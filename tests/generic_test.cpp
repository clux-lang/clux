/*
 * M7 泛型：前端语法、格式化、单态化（泛型函数 + 显式实参 + 调用点推导）。
 *
 * 覆盖：
 *   - 五种声明的 `[T, U = i32, N: comptime u64, ...Args]` 解析形状
 *   - 参数包位置 / cunion 泛型 的语法拒绝
 *   - clux format 保留泛型参数且幂等
 *   - 泛型函数端到端：显式实例化、推导、函数值、递归、所有权不对称
 *   - 负例诊断：实参数量 / 推导冲突 / 无法推导 / 函数体内类型错误
 */

#include <gtest/gtest.h>

#include <cstring>
#include <string>

extern "C" {
#include "core/allocator.h"
#include "core/arena.h"
#include "core/stream.h"
#include "core/vec.h"
#include "parser/ast_enum_def.h"
#include "parser/ast_func_def.h"
#include "parser/ast_generic_param.h"
#include "parser/ast_node.h"
#include "parser/ast_pattern.h"
#include "parser/ast_program.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_type_def.h"
#include "parser/ast_union_def.h"
#include "parser/fmt.h"
#include "parser/lexer.h"
#include "parser/parser.h"
}

#include "test_common.h"

namespace {

struct Parsed {
    allocator_t *alloc  = nullptr;
    arena_t     *arena  = nullptr;
    lexer_t     *lexer  = nullptr;
    vec_t       *tokens = nullptr;
    char        *srcbuf = nullptr;
    ast_node_t  *program = nullptr;

    ~Parsed() {
        if (lexer) lexer_close(&lexer);
        if (tokens) vec_free(alloc, &tokens);
        if (arena) arena_destroy(alloc, &arena);
        if (srcbuf) free(srcbuf);
        if (alloc) EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc);
    }
};

static void parse_into(Parsed &p, const char *src) {
    p.alloc = create_allocator(malloc, free);
    ASSERT_NE(p.alloc, nullptr);
    p.arena = arena_new_default(p.alloc);
    ASSERT_NE(p.arena, nullptr);

    size_t n = strlen(src);
    p.srcbuf = (char *)malloc(n + 1);
    ASSERT_NE(p.srcbuf, nullptr);
    memcpy(p.srcbuf, src, n + 1);

    stream_source_t mem = stream_source_mem(p.alloc, p.srcbuf, n, false);
    istream_t *stream = istream_open(p.alloc, mem);
    ASSERT_NE(stream, nullptr);
    p.lexer = lexer_create(p.alloc, stream, "generic_test.cx");
    ASSERT_NE(p.lexer, nullptr);

    p.tokens = vec_new(p.alloc, true);
    for (;;) {
        token_t *t = lexer_next(p.lexer);
        token_kind_t k = token_get_kind(t);
        vec_push(p.tokens, p.alloc, t);
        if (k == TOKEN_TYPE_EOF || k == TOKEN_TYPE_ERROR) break;
    }

    parser_t *parser = parser_create(p.alloc, p.arena, p.tokens);
    ASSERT_NE(parser, nullptr);
    p.program = parser_parse(parser);
    parser_destroy(&parser);
}

static std::string fmt_source(const char *src) {
    allocator_t *a = create_allocator(malloc, free);
    EXPECT_NE(a, nullptr);
    char *out = fmt_format_source(a, src, strlen(src), nullptr);
    std::string result = out ? out : "";
    if (out) allocator_free(a, (void **)&out);
    EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
    return result;
}

static ast_node_t *find_decl(ast_node_t *program, ast_kind_t kind) {
    if (!program || program->kind != AST_PROGRAM) return nullptr;
    for (ast_node_t *d = ((ast_program_t *)program)->funcs; d; d = d->next)
        if (d->kind == kind) return d;
    return nullptr;
}

} // namespace

/* ================================================================ */
/* 解析                                                              */
/* ================================================================ */

TEST(GenericParse, FunctionParamsShape) {
    Parsed p;
    ASSERT_NO_FATAL_FAILURE(parse_into(
        p,
        "func f[T, U = i32, N: comptime u64](x: T): T { return x; }\n"));
    ASSERT_NE(p.program, nullptr);
    ast_node_t *d = find_decl(p.program, AST_FUNC_DEF);
    ASSERT_NE(d, nullptr);
    ast_func_def_t *fn = (ast_func_def_t *)d;
    ASSERT_EQ(fn->generic.count, 3u);
    ast_generic_param_t *gp0 = (ast_generic_param_t *)fn->generic.first;
    ASSERT_NE(gp0, nullptr);
    EXPECT_STREQ(std::string(gp0->name.ptr, gp0->name.len).c_str(), "T");
    EXPECT_FALSE(gp0->is_comptime);
    EXPECT_FALSE(gp0->is_pack);
    EXPECT_EQ(gp0->default_expr, nullptr);

    ast_generic_param_t *gp1 = (ast_generic_param_t *)gp0->base.next;
    ASSERT_NE(gp1, nullptr);
    EXPECT_STREQ(std::string(gp1->name.ptr, gp1->name.len).c_str(), "U");
    EXPECT_NE(gp1->default_expr, nullptr);

    ast_generic_param_t *gp2 = (ast_generic_param_t *)gp1->base.next;
    ASSERT_NE(gp2, nullptr);
    EXPECT_STREQ(std::string(gp2->name.ptr, gp2->name.len).c_str(), "N");
    EXPECT_TRUE(gp2->is_comptime);
    EXPECT_NE(gp2->type_expr, nullptr);
}

TEST(GenericParse, TypeDeclKindsShape) {
    Parsed p;
    ASSERT_NO_FATAL_FAILURE(parse_into(
        p,
        "struct Vec[T] { data: T; }\n"
        "enum Option[T]: i32 { Some = 1, None = 0 }\n"
        "union Res[T, E] { Ok: T; Err: E; }\n"
        "type Pair[T, U] = <T, U>;\n"));
    ASSERT_NE(p.program, nullptr);

    ast_struct_def_t *sd = (ast_struct_def_t *)find_decl(p.program, AST_STRUCT_DEF);
    ASSERT_NE(sd, nullptr);
    EXPECT_EQ(sd->generic.count, 1u);

    ast_enum_def_t *ed = (ast_enum_def_t *)find_decl(p.program, AST_ENUM_DEF);
    ASSERT_NE(ed, nullptr);
    EXPECT_EQ(ed->generic.count, 1u);

    ast_union_def_t *ud = (ast_union_def_t *)find_decl(p.program, AST_UNION_DEF);
    ASSERT_NE(ud, nullptr);
    EXPECT_EQ(ud->generic.count, 2u);

    ast_type_def_t *td = (ast_type_def_t *)find_decl(p.program, AST_TYPE_DEF);
    ASSERT_NE(td, nullptr);
    EXPECT_EQ(td->generic.count, 2u);
}

TEST(GenericParse, PackMustBeLast) {
    Parsed p;
    ASSERT_NO_FATAL_FAILURE(
        parse_into(p, "func f[...Args, T](x: T): void {}\n"));
    /* 语法错误 → parse_program 返回 AST_ERROR。 */
    ASSERT_NE(p.program, nullptr);
    EXPECT_EQ(p.program->kind, AST_ERROR);
}

TEST(GenericParse, CunionCannotBeGeneric) {
    Parsed p;
    ASSERT_NO_FATAL_FAILURE(
        parse_into(p, "cunion U[T] { a: T; }\n"));
    ASSERT_NE(p.program, nullptr);
    EXPECT_EQ(p.program->kind, AST_ERROR);
}

TEST(GenericParse, ConstraintPatternShape) {
    Parsed p;
    ASSERT_NO_FATAL_FAILURE(
        parse_into(p, "func f[T extends ?*? | [?]i32](x: T): void {}\n"));
    ASSERT_NE(p.program, nullptr);
    ASSERT_EQ(p.program->kind, AST_PROGRAM);
    ast_node_t *d = find_decl(p.program, AST_FUNC_DEF);
    ASSERT_NE(d, nullptr);
    ast_func_def_t *fn = (ast_func_def_t *)d;
    ast_generic_param_t *gp = (ast_generic_param_t *)fn->generic.first;
    ASSERT_NE(gp, nullptr);
    ASSERT_NE(gp->constraint, nullptr);
    EXPECT_EQ(gp->constraint->kind, AST_PAT_OR);
}

TEST(GenericParse, DuplicateParamRejected) {
    Parsed p;
    ASSERT_NO_FATAL_FAILURE(
        parse_into(p, "func f[T, T](x: T): void {}\n"));
    ASSERT_NE(p.program, nullptr);
    EXPECT_EQ(p.program->kind, AST_ERROR);
}

/* ================================================================ */
/* 格式化                                                            */
/* ================================================================ */

TEST(GenericFmt, PreservesAllDeclarationKindsAndIsIdempotent) {
    const char *src =
        "func id[T, U = i32, N: comptime u64](x: T): T{return x;}\n"
        "struct Vec[T]{data:T;}\n"
        "enum Option[T]:i32{Some=1,None=0}\n"
        "union Res[T,E]{Ok:T;Err:E;}\n"
        "type Pair[T,U]=<T,U>;\n";
    std::string once = fmt_source(src);
    EXPECT_NE(once.find("func id[T, U = i32, N: comptime u64](x: T): T {"),
              std::string::npos)
        << once;
    EXPECT_NE(once.find("struct Vec[T] {"), std::string::npos) << once;
    EXPECT_NE(once.find("enum Option[T]: i32 {"), std::string::npos) << once;
    EXPECT_NE(once.find("union Res[T, E] {"), std::string::npos) << once;
    EXPECT_NE(once.find("type Pair[T, U] = <T, U>;"), std::string::npos) << once;
    EXPECT_EQ(fmt_source(once.c_str()), once);
}

/* ================================================================ */
/* 端到端                                                            */
/* ================================================================ */

TEST(GenericRun, ExplicitInstantiationAndNestedCalls) {
    clux_expect_ok(
        "func id[T](x: T): T { return x; }\n"
        "func first[T, U](a: T, b: U): T { return a; }\n"
        "func twice[T](x: T): T { return id[T](id[T](x)); }\n"
        "func main(): i32 {\n"
        "    var a: i32 = id[i32](41);\n"
        "    var b: f64 = id[f64](2.5);\n"
        "    var c: i32 = first[i32, f64](7, 1.5);\n"
        "    var d: i32 = twice[i32](a);\n"
        "    return c + d - 48;\n"
        "}\n",
        "generic_explicit");
}

TEST(GenericRun, CallSiteInference) {
    clux_expect_ok(
        "func id[T](x: T): T { return x; }\n"
        "func add[T](a: T, b: T): T { return a + b; }\n"
        "func first[T, U](a: T, b: U): T { return a; }\n"
        "func main(): i32 {\n"
        "    var a: i32 = id(41);\n"
        "    var b: f64 = id(2.5);\n"
        "    var c: i32 = add(20, 22);\n"
        "    var d: i32 = first(7, 1.5);\n"
        "    return a + c - d - 76;\n"
        "}\n",
        "generic_infer");
}

TEST(GenericRun, FunctionValueAndRecursion) {
    clux_expect_ok(
        "func id[T](x: T): T { return x; }\n"
        "func fact[T](x: T): T {\n"
        "    if (x <= 1) { return 1; }\n"
        "    return x * fact[T](x - 1);\n"
        "}\n"
        "func main(): i32 {\n"
        "    var f: func(i32) -> i32 = id[i32];\n"
        "    var a = f(21);\n"
        "    var b = fact[i32](5);\n"
        "    return a + b - 141;\n"
        "}\n",
        "generic_value");
}

TEST(GenericRun, OwnershipInferenceIsAsymmetric) {
    clux_expect_ok(
        "func peek[T](p: ref *T): T { return p.*; }\n"
        "func take[T](p: own *T): T { return p.*; }\n"
        "func main(): i32 {\n"
        "    var v: i32 = 9;\n"
        "    var r: ref *i32 = v.&;\n"
        "    var a = peek(r);\n"
        "    var o: own *i32 = new i32{ 11 };\n"
        "    var b = take(move(o));\n"
        "    var o2: own *i32 = new i32{ 3 };\n"
        "    var c = peek(o2);\n"
        "    return a + b + c - 23;\n"
        "}\n",
        "generic_ownership");
}

TEST(GenericRun, DefaultTypeArgument) {
    clux_expect_ok(
        "func size_of[T = i32](): u64 { return sizeof(T); }\n"
        "func main(): i32 {\n"
        "    var a = size_of();\n"
        "    var b = size_of[f64]();\n"
        "    return a as i32 + b as i32 - 12;\n"
        "}\n",
        "generic_default");
}

TEST(GenericRun, ForeachInGenericBody) {
    clux_expect_ok(
        "func count[T](xs: ref []T): u64 {\n"
        "    var n: u64 = 0u64;\n"
        "    foreach (var x of xs) { n = n + 1u64; }\n"
        "    return n;\n"
        "}\n"
        "func main(): i32 {\n"
        "    var a = .[3]i32{ 1, 2, 3 };\n"
        "    var n = count[i32](a[:]);\n"
        "    return n as i32 - 3;\n"
        "}\n",
        "generic_foreach");
}

/* ---- P4：泛型 struct / enum / union / 别名 ---- */

TEST(GenericType, StructFieldsMethodsAndStaticMethod) {
    clux_expect_ok(
        "struct Box[T] {\n"
        "    value: T;\n"
        "    func create(v: T): Box[T] { var b: Box[T] = .{ .value = v }; return b; }\n"
        "    func get(self: ref *const Box[T]): T { return self.value; }\n"
        "    func set(self: ref *Box[T], v: T): void { self.value = v; }\n"
        "}\n"
        "func main(): i32 {\n"
        "    var a: Box[i32] = .Box[i32]{ .value = 1 };\n"
        "    a.set(2);\n"
        "    var b = Box[i32]::create(3);\n"
        "    return a.get() + b.get() - 5;\n"
        "}\n",
        "generic_struct_methods");
}

TEST(GenericType, StructInsideSliceAndTuple) {
    clux_expect_ok(
        "struct Box[T] { value: T; }\n"
        "type Pair[T, U] = <T, U>;\n"
        "type Triple[T] = [3]T;\n"
        "func main(): i32 {\n"
        "    var p: Pair[i32, str] = .<i32, str>{ 7, \"x\" };\n"
        "    var t: Triple[i32] = .[3]i32{ 1, 2, 3 };\n"
        "    var b: Box[i32] = .{ .value = p[0] + t[2] };\n"
        "    return b.value - 10;\n"
        "}\n",
        "generic_struct_alias");
}

TEST(GenericType, UnionAndEnumInstances) {
    clux_expect_ok(
        "union Res[T, E] { Ok: T; Err: E; }\n"
        "enum Tag[Marker]: i32 { A = 1, B = 2 }\n"
        "func main(): i32 {\n"
        "    var r: Res[i32, str] = .Res[i32, str]{ .Ok = 7 };\n"
        "    var t: Tag[i32] = Tag[i32]::A;\n"
        "    return r.Ok + (t as i32) - 8;\n"
        "}\n",
        "generic_union_enum");
}

TEST(GenericType, NestedAndRecursiveInstances) {
    clux_expect_ok(
        "struct Box[T] { value: T; }\n"
        "struct Node[T] { value: T; next: ?ref *const Node[T]; }\n"
        "func main(): i32 {\n"
        "    var b: Box[Box[i32]] = .{ .value = .{ .value = 4 } };\n"
        "    var n: Node[i32] = .{ .value = 3, "
        ".next = .?ref *const Node[i32]{ nil } };\n"
        "    return b.value.value + n.value - 7;\n"
        "}\n",
        "generic_nested_recursive");
}

TEST(GenericType, StaticAttributePerInstance) {
    clux_expect_ok(
        "struct Counter[T] {\n"
        "    var n: i32 = 0;\n"
        "    func bump(self: ref *Counter[T]): void {\n"
        "        Counter[T]::n = Counter[T]::n + 1;\n"
        "    }\n"
        "    func value(self: ref *const Counter[T]): i32 {\n"
        "        return Counter[T]::n;\n"
        "    }\n"
        "}\n"
        "func main(): i32 {\n"
        "    var c: Counter[i32] = .{};\n"
        "    c.bump();\n"
        "    c.bump();\n"
        "    return c.value() - 2;\n"
        "}\n",
        "generic_static_attr");
}

TEST(GenericType, InferenceThroughGenericStructParameter) {
    clux_expect_ok(
        "struct Box[T] { value: T; }\n"
        "func wrap[T](v: T): Box[T] { return .Box[T]{ .value = v }; }\n"
        "func unwrap[T](b: Box[T]): T { return b.value; }\n"
        "func read[T](b: ref *Box[T]): T { return b.value; }\n"
        "func main(): i32 {\n"
        "    var b = wrap(5);\n"
        "    var v = unwrap(b) + read(b.&);\n"
        "    return v - 10;\n"
        "}\n",
        "generic_struct_infer");
}

TEST(GenericType, OwnFieldAndNestedByValue) {
    clux_expect_ok(
        "struct Box[T] { v: T; }\n"
        "struct Holder[T] { b: Box[T]; }\n"
        "struct Own[T] {\n"
        "    p: own *T;\n"
        "    func get(self: ref *const Own[T]): T { return self.p.*; }\n"
        "}\n"
        "func main(): i32 {\n"
        "    var h: Holder[i32] = .{ .b = .{ .v = 2 } };\n"
        "    var o: own Own[i32] = .Own[i32]{ .p = new i32{ 8 } };\n"
        "    return h.b.v + o.get() - 10;\n"
        "}\n",
        "generic_own_field");
}

/* ---- P5：comptime 值参数 ---- */

TEST(GenericValueParam, ExplicitInferenceAndDefault) {
    clux_expect_ok(
        "func size_of_n[T, N: comptime u64](): u64 { return N * sizeof(T); }\n"
        "func first_arr[T, N: comptime u64](a: [N]T): T { return a[0]; }\n"
        "func sum_len[T, N: comptime u64](a: [N]T): u64 { return N + sizeof(T); }\n"
        "func with_default[T = i32, N: comptime u64 = 5](): u64 {\n"
        "    return N + sizeof(T);\n"
        "}\n"
        "func main(): i32 {\n"
        "    var a = size_of_n[i32, 3]();\n"
        "    var arr = .[4]i32{ 7, 8, 9, 10 };\n"
        "    var x = first_arr(arr);\n"
        "    var n = sum_len(arr);\n"
        "    var d = with_default();\n"
        "    return (a as i32) + x + (n as i32) + (d as i32) - 31;\n"
        "}\n",
        "generic_value_param");
}

TEST(GenericValueParam, PartitionByValueArgument) {
    /* 不同 N 必须生成不同实例（mangling 含值）。 */
    clux_expect_ok(
        "func first_arr[T, N: comptime u64](a: [N]T): T { return a[0]; }\n"
        "func main(): i32 {\n"
        "    var a3 = .[3]i32{ 1, 2, 3 };\n"
        "    var a4 = .[4]i32{ 4, 5, 6, 7 };\n"
        "    var x = first_arr(a3);\n"
        "    var y = first_arr(a4);\n"
        "    return x + y - 5;\n"
        "}\n",
        "generic_value_partition");
}

TEST(GenericValueParam, TypeWithValueParameter) {
    clux_expect_ok(
        "struct Matrix[T, N: comptime u64] { data: [N]T; }\n"
        "func main(): i32 {\n"
        "    var m: Matrix[i32, 3] = .{ .data = .[3]i32{ 1, 2, 3 } };\n"
        "    return m.data[2] - 3;\n"
        "}\n",
        "generic_matrix");
}

TEST(GenericValueParam, ValueParameterInBodyTypeAndFill) {
    clux_expect_ok(
        "func fill[T, N: comptime u64](v: T): [N]T {\n"
        "    var a: [N]T = .[N]T{ <v, N> };\n"
        "    if (N > 0u64) { return a; }\n"
        "    return a;\n"
        "}\n"
        "func main(): i32 {\n"
        "    var a = fill[i32, 4](9);\n"
        "    return a[0] + a[3] - 18;\n"
        "}\n",
        "generic_value_body");
}

/* ---- P5b：参数包 ---- */

TEST(GenericPack, ExplicitInferredAndLen) {
    clux_expect_ok(
        "func show[...Args](tag: str, args: ...Args): void {}\n"
        "func count[...Args](tag: str, args: ...Args): u64 { return len(args); }\n"
        "func main(): i32 {\n"
        "    show[i32, str](\"a\", 1, \"x\");\n"
        "    show(\"b\", 2, \"y\");\n"
        "    var n = count[i32, str](\"a\", 1, \"x\");\n"
        "    var m = count(\"b\");\n"
        "    return (n as i32) + (m as i32) - 2;\n"
        "}\n",
        "generic_pack_basic");
}

TEST(GenericPack, FixedParamsAndPackInference) {
    clux_expect_ok(
        "func f[T, ...Args](x: T, args: ...Args): T { return x; }\n"
        "func nargs[T, ...Args](x: T, args: ...Args): u64 { return len(args); }\n"
        "func main(): i32 {\n"
        "    var a = f[i32, str, f64](1, \"a\", 2.5);\n"
        "    var b = f[i32](1, \"a\", 2.5);\n"
        "    var c = f(1, \"a\");\n"
        "    var n = nargs(7, \"a\", 1.5);\n"
        "    return a + b + c + (n as i32) - 5;\n"
        "}\n",
        "generic_pack_fixed");
}

TEST(GenericPack, EmptyPackAndTupleAggregation) {
    clux_expect_ok(
        "func first_or[...Args](args: ...Args): i32 { return 7; }\n"
        "func size[...Args](args: ...Args): u64 { return len(args); }\n"
        "func main(): i32 {\n"
        "    var a = first_or();\n"
        "    var n = size();\n"
        "    return a + (n as i32) - 7;\n"
        "}\n",
        "generic_pack_empty");
}

/* ---- P6a：extends 结构约束 ---- */

TEST(GenericConstraint, PointerOwnershipWildcards) {
    clux_expect_ok(
        "func read_ptr[T extends ?*?](p: T): i32 { return p.*; }\n"
        "func read_own[T extends own *?](p: T): i32 { return p.*; }\n"
        "func main(): i32 {\n"
        "    var v: i32 = 5;\n"
        "    var r: ref *i32 = v.&;\n"
        "    var x = read_ptr(r);\n"
        "    var o: own *i32 = new i32{ 7 };\n"
        "    var y = read_own[own *i32](move(o));\n"
        "    return x + y - 12;\n"
        "}\n",
        "generic_constraint_ptr");
}

TEST(GenericConstraint, ArrayTupleGenericStructFunctionOr) {
    clux_expect_ok(
        "struct Box[T] { value: T; }\n"
        "func arr_len[T extends [?]i32](a: T): u64 { return len(a); }\n"
        "func tuple_first[T extends <?>](t: T): i32 { return t[0]; }\n"
        "func boxed[T extends Box[?]](b: T): i32 { return 7; }\n"
        "func fn_ok[T extends func(i32)->?](f: T): i32 { return f(1); }\n"
        "func as_num[T extends i32 | f64](x: T): T { return x; }\n"
        "func main(): i32 {\n"
        "    var a = .[3]i32{ 1, 2, 3 };\n"
        "    var t = .<i32, str>{ 5, \"x\" };\n"
        "    var b: Box[i32] = .{ .value = 9 };\n"
        "    var f: func(i32) -> i32 = func(x: i32): i32 { return x; };\n"
        "    var n1 = arr_len(a);\n"
        "    var n2 = tuple_first(t);\n"
        "    var n3 = as_num(2.5);\n"
        "    var n4 = boxed(b);\n"
        "    var n5 = fn_ok(f);\n"
        "    return (n1 as i32) + n2 + (n3 as i32) + n4 + n5 - 18;\n"
        "}\n",
        "generic_constraint_forms");
}

TEST(GenericConstraint, StructFieldsAndConstQualifier) {
    clux_expect_ok(
        "struct Point { x: i32; y: i32; }\n"
        "struct Same { x: i32; y: i32; }\n"
        "func sx[T extends Point](p: T): i32 { return p.x; }\n"
        "func both[T extends const ? & ?](x: T): i32 { return 1; }\n"
        "func any_ok[T extends ?](): i32 { return 2; }\n"
        "func main(): i32 {\n"
        "    var p: Point = .{ .x = 4, .y = 5 };\n"
        "    var q: Same = .{ .x = 6, .y = 7 };\n"
        "    var c: const i32 = 9;\n"
        "    var a = sx(p) + sx(q);\n"
        "    var b = both(c);\n"
        "    var d = any_ok[void]();\n"
        "    return a + b + d - 13;\n"
        "}\n",
        "generic_constraint_struct");
}

TEST(GenericConstraint, ConstraintFmtIdempotent) {
    const char *src =
        "func f[T extends ?*? | [?]i32](x: T): void {}\n"
        "func g[T extends own *? & const ?](x: T): void {}\n"
        "struct Box[T] { v: T; }\n"
        "func h[T extends Box[?] | <i32, ?>, U extends []?](x: T, y: U): void {}\n";
    std::string once = fmt_source(src);
    EXPECT_NE(once.find("T extends (?*? | [?]i32)"), std::string::npos) << once;
    EXPECT_EQ(fmt_source(once.c_str()), once);
}

/* ---- P6b：interface ---- */

TEST(Interface, DeclareImplementAndConstrain) {
    clux_expect_ok(
        "interface IWriter[T] {\n"
        "    var fd: i32;\n"
        "    func write(self: ref *T, data: ref []u8);\n"
        "}\n"
        "struct Writer implements IWriter[Writer] {\n"
        "    fd: i32;\n"
        "    func write(self: ref *Writer, data: ref []u8): void {\n"
        "        self.fd = self.fd + 1;\n"
        "    }\n"
        "}\n"
        "func serialize[T extends IWriter[T]](w: ref *T): i32 { return w.fd; }\n"
        "func main(): i32 {\n"
        "    var w: Writer = .{ .fd = 3 };\n"
        "    var d: [2]u8 = .[2]u8{ 65 as u8, 66 as u8 };\n"
        "    w.write(d[:]);\n"
        "    return serialize(w.&) - 4;\n"
        "}\n",
        "interface_basic");
}

TEST(Interface, GenericStructImplementsAndConstraint) {
    clux_expect_ok(
        "interface IWriter[T] {\n"
        "    var fd: i32;\n"
        "    func write(self: ref *T, data: ref []u8);\n"
        "}\n"
        "struct Wrapper[T] implements IWriter[Wrapper[T]] {\n"
        "    fd: i32;\n"
        "    value: T;\n"
        "    func write(self: ref *Wrapper[T], data: ref []u8): void {\n"
        "        self.fd = self.fd + 1;\n"
        "    }\n"
        "}\n"
        "func fd_of[T extends IWriter[T]](w: ref *T): i32 { return w.fd; }\n"
        "func main(): i32 {\n"
        "    var w: Wrapper[i32] = .{ .fd = 10, .value = 1 };\n"
        "    var d: [1]u8 = .[1]u8{ 65 as u8 };\n"
        "    w.write(d[:]);\n"
        "    return fd_of(w.&) - 11;\n"
        "}\n",
        "interface_generic_struct");
}

TEST(Interface, MultipleInterfacesWithAmpersand) {
    clux_expect_ok(
        "interface IW[T] { func w(self: ref *T); }\n"
        "interface IR[T] { func r(self: ref *T): i32; }\n"
        "struct S implements IW[S] & IR[S] {\n"
        "    func w(self: ref *S): void {}\n"
        "    func r(self: ref *S): i32 { return 4; }\n"
        "}\n"
        "func both[T extends IW[T] & IR[T]](s: ref *T): i32 {\n"
        "    s.w();\n"
        "    return s.r();\n"
        "}\n"
        "func main(): i32 {\n"
        "    var s: S = .{};\n"
        "    return both(s.&) - 4;\n"
        "}\n",
        "interface_amp");
}

TEST(Interface, AmpersandFmtIdempotent) {
    const char *src =
        "interface IW[T]{func w(self:ref *T);}\n"
        "interface IR[T]{func r(self:ref *T):i32;}\n"
        "struct S implements IW[S]&IR[S]{func w(self:ref *S):void{}"
        "func r(self:ref *S):i32{return 4;}}\n";
    std::string once = fmt_source(src);
    EXPECT_NE(once.find("implements IW[S] & IR[S] {"), std::string::npos)
        << once;
    EXPECT_EQ(fmt_source(once.c_str()), once);
}

TEST(Interface, MemberCopyArgsFmtIdempotent) {
    const char *src =
        "interface IW[T]{func w(self:ref *T);}\n"
        "interface IRW[T]{...IW[T];}\n";
    std::string once = fmt_source(src);
    EXPECT_NE(once.find("...IW[T];"), std::string::npos) << once;
    EXPECT_EQ(fmt_source(once.c_str()), once);
}

TEST(Interface, FmtIdempotent) {
    const char *src =
        "interface I[T]{var fd:i32;func w(self:ref *T,data:ref []u8);}\n"
        "struct S implements I[S]{fd:i32;func w(self:ref *S,data:ref []u8):void{}}\n";
    std::string once = fmt_source(src);
    EXPECT_NE(once.find("interface I[T] {"), std::string::npos) << once;
    EXPECT_NE(once.find("struct S implements I[S] {"), std::string::npos) << once;
    EXPECT_NE(once.find("func w(self: ref *S, data: ref []u8): void {}"),
              std::string::npos)
        << once;
    EXPECT_EQ(fmt_source(once.c_str()), once);
}

TEST(Interface, MemberCopyFromBase) {
    clux_expect_ok(
        "interface IBase { var fd: i32; }\n"
        "interface IW[T] { ...IBase; func w(self: ref *T); }\n"
        "struct W implements IW[W] {\n"
        "    fd: i32;\n"
        "    func w(self: ref *W): void {}\n"
        "}\n"
        "func main(): i32 {\n"
        "    var w: W = .{ .fd = 1 };\n"
        "    w.w();\n"
        "    return w.fd - 1;\n"
        "}\n",
        "interface_member_copy");
}

TEST(Interface, GenericMemberCopyWithArgs) {
    clux_expect_ok(
        "interface IW[T] { func w(self: ref *T); }\n"
        "interface IR[T] { func r(self: ref *T): i32; }\n"
        "interface IRW[T] { ...IW[T]; ...IR[T]; }\n"
        "struct S implements IRW[S] {\n"
        "    func w(self: ref *S): void {}\n"
        "    func r(self: ref *S): i32 { return 4; }\n"
        "}\n"
        "func use[T extends IRW[T]](s: ref *T): i32 {\n"
        "    s.w();\n"
        "    return s.r();\n"
        "}\n"
        "func main(): i32 {\n"
        "    var s: S = .{};\n"
        "    return use(s.&) - 4;\n"
        "}\n",
        "interface_generic_copy");
}

/* ---- P4b：泛型方法（方法自身带类型参数） ---- */

TEST(GenericMethod, MapIntoOnGenericStruct) {
    clux_expect_ok(
        "struct Box[T] {\n"
        "    value: T;\n"
        "    func get(self: ref *const Box[T]): T { return self.value; }\n"
        "    func map_into[U](self: ref *const Box[T], f: func(T) -> U): Box[U] {\n"
        "        var b: Box[U] = .{ .value = f(self.value) };\n"
        "        return b;\n"
        "    }\n"
        "}\n"
        "func inc(x: i32): i32 { return x + 1; }\n"
        "func main(): i32 {\n"
        "    var b: Box[i32] = .{ .value = 4 };\n"
        "    var c = b.map_into(inc);\n"
        "    return c.get() - 5;\n"
        "}\n",
        "generic_method_map");
}

TEST(GenericMethod, NonGenericStructAndFunctionLiteral) {
    clux_expect_ok(
        "struct IntBox {\n"
        "    value: i32;\n"
        "    func convert[U](self: ref *IntBox, f: func(i32) -> U): U {\n"
        "        return f(self.value);\n"
        "    }\n"
        "}\n"
        "func main(): i32 {\n"
        "    var ib: IntBox = .{ .value = 9 };\n"
        "    var d = ib.convert(func(x: i32): i32 { return x * 2; });\n"
        "    return d - 18;\n"
        "}\n",
        "generic_method_nongeneric");
}

TEST(GenericMethod, StaticGenericMethod) {
    clux_expect_ok(
        "struct Box[T] {\n"
        "    value: T;\n"
        "    func create[U](v: U): Box[U] {\n"
        "        var b: Box[U] = .{ .value = v };\n"
        "        return b;\n"
        "    }\n"
        "}\n"
        "func main(): i32 {\n"
        "    var b = Box[i32]::create(7);\n"
        "    return b.value - 7;\n"
        "}\n",
        "generic_method_static");
}

TEST(GenericPack, TupleTypeExpansion) {
    clux_expect_ok(
        "func passthrough[...Args](args: ...Args): <...Args> { return .{ ...args }; }\n"
        "func count[...Args](args: ...Args): u64 {\n"
        "    var copy: <...Args> = .{ ...args };\n"
        "    return len(copy);\n"
        "}\n"
        "func main(): i32 {\n"
        "    var t = passthrough[i32, str](1, \"x\");\n"
        "    var n = count[i32, str, f64](1, \"x\", 2.5);\n"
        "    return t[0] + (n as i32) - 4;\n"
        "}\n",
        "generic_pack_tuple_type");
}

TEST(GenericPack, TupleExpansionFmtIdempotent) {
    const char *src =
        "func f[...Args](args:...Args):<...Args>{return args;}\n"
        "func g[...Args](args:...Args):void{var t: <...Args> = args;}\n";
    std::string once = fmt_source(src);
    EXPECT_NE(once.find("): <...Args> {"), std::string::npos) << once;
    EXPECT_NE(once.find("var t: <...Args> = args;"), std::string::npos) << once;
    EXPECT_EQ(fmt_source(once.c_str()), once);
}

TEST(GenericPack, AliasParameterPack) {
    clux_expect_ok(
        "type Tuple[...Args] = <...Args>;\n"
        "type IntStr = Tuple[i32, str];\n"
        "type First[T, ...Args] = T;\n"
        "func main(): i32 {\n"
        "    var a: IntStr = .<i32, str>{ 1, \"x\" };\n"
        "    var b: First[i32, str, f64] = 3;\n"
        "    var e: Tuple[] = .{};\n"
        "    return a[0] + b + (len(e) as i32) - 4;\n"
        "}\n",
        "generic_pack_alias");
}

TEST(GenericPack, AliasPackFmtIdempotent) {
    const char *src =
        "type Tuple[...Args]=<...Args>;\n"
        "type First[T,...Args]=T;\n";
    std::string once = fmt_source(src);
    EXPECT_NE(once.find("type Tuple[...Args] = <...Args>;"), std::string::npos)
        << once;
    EXPECT_NE(once.find("type First[T, ...Args] = T;"), std::string::npos)
        << once;
    EXPECT_EQ(fmt_source(once.c_str()), once);
}

TEST(GenericPack, StructParameterPack) {
    clux_expect_ok(
        "struct TupleStruct[...Args] { data: <...Args>; }\n"
        "func main(): i32 {\n"
        "    var t: TupleStruct[i32, str] = .{ .data = .<i32, str>{ 4, \"x\" } };\n"
        "    var e: TupleStruct[] = .{ .data = .{} };\n"
        "    return t.data[0] + (len(e.data) as i32) - 4;\n"
        "}\n",
        "generic_pack_struct");
}

/* ---- P9：comptime 值参数包 ---- */

TEST(GenericValuePack, IndexAndLen) {
    clux_expect_ok(
        "func total[T, ...N: comptime u64](x: T): u64 {\n"
        "    return N[0] + N[1] + sizeof(T);\n"
        "}\n"
        "func empty[...N: comptime u64](): u64 { return len(N); }\n"
        "func main(): i32 {\n"
        "    var a = total[i32, 2u64, 5u64](1);\n"
        "    var b = empty[]();\n"
        "    return (a + b - 11u64) as i32;\n"
        "}\n",
        "generic_value_pack_index");
}

TEST(GenericValuePack, ArrayLengthFromPack) {
    clux_expect_ok(
        "func build_arr[...N: comptime u64](): [N[0]]i32 {\n"
        "    var a: [N[0]]i32 = .[N[0]]i32{ 0, 0, 0 };\n"
        "    a[0] = 7;\n"
        "    return a;\n"
        "}\n"
        "func main(): i32 {\n"
        "    var a = build_arr[3u64]();\n"
        "    return a[0] - 7 + (len(a) as i32) - 3;\n"
        "}\n",
        "generic_value_pack_array");
}

TEST(GenericValuePack, FmtIdempotent) {
    const char *src =
        "func f[...N: comptime u64](): u64 { return N[0] + len(N); }\n";
    std::string once = fmt_source(src);
    EXPECT_NE(once.find("[...N: comptime u64]"), std::string::npos) << once;
    EXPECT_EQ(fmt_source(once.c_str()), once);
}

/* ================================================================ */
/* 负例                                                              */
/* ================================================================ */

TEST(GenericReject, TypeArgumentCountMismatch) {
    clux_expect_reject(
        "func first[T, U](a: T, b: U): T { return a; }\n"
        "func main(): i32 { var x = first[i32, f64, str](1, 2); return 0; }\n",
        "expects 2 generic argument(s), got 3", "generic_bad_count");
}

TEST(GenericReject, InconsistentInference) {
    clux_expect_reject(
        "func add[T](a: T, b: T): T { return a + b; }\n"
        "func main(): i32 { var x = add(1, 2.5); return 0; }\n",
        "inconsistent type argument for 'T'", "generic_bad_infer");
}

TEST(GenericReject, CannotDeduce) {
    clux_expect_reject(
        "func make_one[T](): u64 { return sizeof(T); }\n"
        "func main(): i32 { var x = make_one(); return 0; }\n",
        "cannot deduce type argument 'T'", "generic_no_deduce");
}

TEST(GenericReject, BodyTypeErrorAtDefinition) {
    clux_expect_reject(
        "func bad[T](x: T): T { return x + 1; }\n"
        "func main(): i32 { var x = bad[str](\"a\"); return 0; }\n",
        "cannot apply '+' to str", "generic_bad_body");
}

TEST(GenericReject, GenericTypeNeedsArguments) {
    clux_expect_reject(
        "struct Vec[T] { data: T; }\n"
        "func main(): i32 { var v: Vec = undefined; return 0; }\n",
        "requires type arguments", "generic_type_bare");
}

TEST(GenericReject, LocalGenericLiteralRejected) {
    clux_expect_reject(
        "func main(): i32 {\n"
        "    var f = func[T](x: T): T { return x; };\n"
        "    return 0;\n"
        "}\n",
        "generic function literals are not implemented yet",
        "generic_local_literal");
}

TEST(GenericReject, InstantiationDepthLimit) {
    clux_expect_reject(
        "func rec[T](): void { rec[own *T](); }\n"
        "func main(): i32 { rec[i32](); return 0; }\n",
        "generic instantiation depth exceeds limit", "generic_depth");
}

TEST(GenericReject, TypeArgumentCountMismatchForType) {
    clux_expect_reject(
        "struct Vec[T] { data: T; }\n"
        "func main(): i32 { var v: Vec[i32, f64] = undefined; return 0; }\n",
        "generic type 'Vec' expects 1 generic argument", "generic_type_count");
}

TEST(GenericReject, ValueArgumentMustBeConstant) {
    clux_expect_reject(
        "func g[N: comptime u64](): u64 { return N; }\n"
        "func main(): i32 { var x = 3; var a = g[x](); return 0; }\n",
        "not a compile-time constant", "generic_value_not_const");
}

TEST(GenericReject, ValueArgumentKindMismatch) {
    clux_expect_reject(
        "func g[N: comptime u64](): u64 { return N; }\n"
        "func main(): i32 { var a = g[i32](); return 0; }\n",
        "expects a value, got a type", "generic_value_kind");
}

TEST(GenericReject, PackExplicitTypeMismatch) {
    clux_expect_reject(
        "func f[...Args](args: ...Args): void {}\n"
        "func main(): i32 { f[str](1); return 0; }\n",
        "cannot convert i32 to str",
        "generic_pack_mismatch");
}

TEST(GenericReject, GenericTypePackRejected) {
    clux_expect_reject(
        "union U[...T] { a: i32; }\n"
        "func main(): i32 { var u: U[i32] = undefined; return 0; }\n",
        "only type aliases and structs may have generic parameter packs", "generic_type_pack");
}

TEST(GenericReject, ConstraintPointerMismatch) {
    clux_expect_reject(
        "func read_ptr[T extends ?*?](p: T): i32 { return p.*; }\n"
        "func main(): i32 { return read_ptr[i32](1); }\n",
        "does not satisfy the constraint", "generic_constraint_bad_ptr");
}

TEST(GenericReject, ConstraintConstMismatch) {
    clux_expect_reject(
        "func only_const[T extends const ?](x: T): i32 { return 1; }\n"
        "func main(): i32 { return only_const(5); }\n",
        "does not satisfy the constraint", "generic_constraint_bad_const");
}

TEST(GenericReject, ConstraintStructExtraField) {
    clux_expect_reject(
        "struct Point { x: i32; y: i32; }\n"
        "struct Pixel { x: i32; y: i32; color: u32; }\n"
        "func sx[T extends Point](p: T): i32 { return p.x; }\n"
        "func main(): i32 {\n"
        "    var p: Pixel = .{ .x = 1, .y = 2, .color = 3u32 };\n"
        "    return sx(p);\n"
        "}\n",
        "does not satisfy the constraint", "generic_constraint_bad_struct");
}

TEST(InterfaceReject, MissingMethod) {
    clux_expect_reject(
        "interface IW[T] { var fd: i32; func w(self: ref *T); }\n"
        "struct Bad implements IW[Bad] { fd: i32; }\n"
        "func main(): i32 { return 0; }\n",
        "missing method 'w'", "interface_missing_method");
}

TEST(InterfaceReject, SignatureMismatch) {
    clux_expect_reject(
        "interface IW[T] { func w(self: ref *T, x: i32); }\n"
        "struct Bad implements IW[Bad] {\n"
        "    func w(self: ref *Bad): void {}\n"
        "}\n"
        "func main(): i32 { return 0; }\n",
        "does not match the interface signature", "interface_bad_sig");
}

TEST(InterfaceReject, InterfaceIsNotAType) {
    clux_expect_reject(
        "interface IW[T] { func w(self: ref *T); }\n"
        "func main(): i32 { var x: IW[i32] = undefined; return 0; }\n",
        "is an interface (a compile-time constraint), not a type",
        "interface_as_type");
}

TEST(InterfaceReject, ConstraintRequiresImplementedInterface) {
    clux_expect_reject(
        "interface IW[T] { var fd: i32; }\n"
        "struct Plain { fd: i32; }\n"
        "func f[T extends IW[T]](w: ref *T): i32 { return w.fd; }\n"
        "func main(): i32 { var p: Plain = .{ .fd = 1 }; return f(p.&); }\n",
        "does not satisfy the constraint", "interface_not_implemented");
}

TEST(InterfaceReject, MemberCopyIsNotInheritance) {
    clux_expect_reject(
        "interface IW[T] { func w(self: ref *T); }\n"
        "interface IRW[T] { ...IW[T]; }\n"
        "struct S implements IRW[S] { func w(self: ref *S): void {} }\n"
        "func g[T extends IW[T]](s: ref *T): i32 { s.w(); return 1; }\n"
        "func main(): i32 { var s: S = .{}; return g(s.&); }\n",
        "does not satisfy the constraint", "interface_copy_not_inherit");
}

TEST(GenericReject, MethodTypeParameterNotDeducible) {
    clux_expect_reject(
        "struct S {\n"
        "    func f[U](self: ref *S): i32 { return 1; }\n"
        "}\n"
        "func main(): i32 {\n"
        "    var s: S = .{};\n"
        "    return s.f();\n"
        "}\n",
        "cannot deduce type argument 'U'", "generic_method_no_deduce");
}

TEST(GenericReject, ValuePackNeedsExplicitArgs) {
    clux_expect_reject(
        "func f[...N: comptime u64](): u64 { return len(N); }\n"
        "func main(): i32 { return f() as i32; }\n",
        "comptime value parameter pack requires explicit generic arguments",
        "value_pack_no_explicit");
}

TEST(GenericReject, ValuePackIndexOutOfRange) {
    clux_expect_reject(
        "func f[...N: comptime u64](): u64 { return N[2]; }\n"
        "func main(): i32 { return f[1u64]() as i32; }\n",
        "index 2 out of range", "value_pack_out_of_range");
}

TEST(GenericReject, ValuePackBareName) {
    clux_expect_reject(
        "func f[...N: comptime u64](): u64 { var a = N; return len(a); }\n"
        "func main(): i32 { return f[1u64, 2u64]() as i32; }\n",
        "is a compile-time sequence", "value_pack_bare");
}

TEST(GenericMethod, ExplicitTypeArguments) {
    clux_expect_ok(
        "struct Box[T] {\n"
        "  value: T;\n"
        "  func map_into[U](self: ref *const Box[T], f: func(T) -> U): Box[U] {\n"
        "    var b: Box[U] = .{ .value = f(self.value) };\n"
        "    return b;\n"
        "  }\n"
        "  func get(self: ref *const Box[T]): T { return self.value; }\n"
        "}\n"
        "func inc(v: i32): i32 { return v + 1; }\n"
        "func main(): i32 {\n"
        "  var b: Box[i32] = .Box[i32]{ .value = 5 };\n"
        "  var c = b.map_into[i32](inc);\n"
        "  return c.get() - 6;\n"
        "}\n",
        "generic_method_explicit");
}

TEST(GenericMethod, StaticExplicitTypeArguments) {
    clux_expect_ok(
        "struct S {\n"
        "  func build[U](x: U): i32 { return 7; }\n"
        "}\n"
        "func main(): i32 { return S::build[i32](1) - 7; }\n",
        "generic_static_method_explicit");
}

TEST(GenericPack, OwnElementMoveAndForward) {
    clux_expect_ok(
        "func first[...Args](args: ...Args): i32 {\n"
        "  var p: own *i32 = move(args[0]);\n"
        "  return p.*;\n"
        "}\n"
        "func forward[...Args](args: ...Args): i32 {\n"
        "  return first(...args);\n"
        "}\n"
        "func main(): i32 {\n"
        "  var x: own *i32 = new i32{ .i32{7} };\n"
        "  return forward[own *i32](move(x)) - 7;\n"
        "}\n",
        "generic_pack_own_forward");
}

TEST(GenericReject, BareParameterPackName) {
    clux_expect_reject(
        "func f[...Args](args: ...Args): <...Args> { return args; }\n"
        "func main(): i32 { var t = f[i32](1); return t[0]; }\n",
        "has no runtime value", "generic_pack_bare_args");
}

TEST(GenericPack, OwnElementTupleReturn) {
    clux_expect_ok(
        "func pack[...Args](args: ...Args): fatal <...Args> { return move(.{ ...args }); }\n"
        "func main(): i32 {\n"
        "  var x: own *i32 = new i32{ .i32{7} };\n"
        "  var t = pack[own *i32](move(x));\n"
        "  return t[0].* - 7;\n"
        "}\n",
        "generic_pack_own_tuple_return");
}

TEST(GenericReject, OwnPackReturnMustBeFatal) {
    clux_expect_reject(
        "func pack[...Args](args: ...Args): <...Args> { return .{ ...args }; }\n"
        "func main(): i32 {\n"
        "  var x: own *i32 = new i32{ .i32{7} };\n"
        "  var t = pack[own *i32](move(x));\n"
        "  return t[0].* - 7;\n"
        "}\n",
        "contains own slots", "generic_pack_own_return_fatal");
}

TEST(GenericPack, OwnPackConstructReturnAutoMove) {
    clux_expect_ok(
        "func pack[...Args](args: ...Args): fatal <...Args> { return .{ ...args }; }\n"
        "func main(): i32 {\n"
        "  var x: own *i32 = new i32{ .i32{7} };\n"
        "  var t = pack[own *i32](move(x));\n"
        "  return t[0].* - 7;\n"
        "}\n",
        "generic_pack_construct_return_auto_move");
}

TEST(GenericPack, NestedOwnAggregate) {
    clux_expect_ok(
        "struct Inner { p: own *i32; }\n"
        "struct Outer { inner: Inner; }\n"
        "func pack[...Args](args: ...Args): fatal <...Args> {\n"
        "  return move(.{ ...args });\n"
        "}\n"
        "func main(): i32 {\n"
        "  var v: own Outer = .Outer{ .inner = .Inner{ .p = new i32{ .i32{7} } } };\n"
        "  var t = pack[Outer](move(v));\n"
        "  return t[0].inner.p.* - 7;\n"
        "}\n",
        "generic_pack_nested_own");
}

TEST(GenericPack, InferredOwnFromFatal) {
    clux_expect_ok(
        "func first[...Args](args: ...Args): i32 {\n"
        "  var p: own *i32 = move(args[0]);\n"
        "  return p.*;\n"
        "}\n"
        "func deref[T](x: T): i32 { return x.*; }\n"
        "func main(): i32 {\n"
        "  var a: own *i32 = new i32{ .i32{7} };\n"
        "  var b: own *i32 = new i32{ .i32{8} };\n"
        "  return first(move(a)) + deref(move(b)) - 15;\n"
        "}\n",
        "generic_pack_infer_own");
}

TEST(GenericReject, OwnArgumentMustUseMove) {
    clux_expect_reject(
        "func first[...Args](args: ...Args): i32 { return 0; }\n"
        "func main(): i32 {\n"
        "  var a: own *i32 = new i32{ .i32{7} };\n"
        "  return first(a);\n"
        "}\n",
        "cannot copy a value holding owned resources", "generic_pack_own_arg");
}

TEST(ComptimeIf, GenericPatternSpecialization) {
    clux_expect_ok(
        "func Test[T](arg: T): void {\n"
        "  comptime if (T extends own *?) {\n"
        "    var a = move(arg);\n"
        "  } else {\n"
        "    var a = arg;\n"
        "  }\n"
        "}\n"
        "func main(): i32 {\n"
        "  var p: own *i32 = new i32{ .i32{7} };\n"
        "  Test(move(p));\n"
        "  Test(5);\n"
        "  return 0;\n"
        "}\n",
        "generic_comptime_pattern_specialize");
}

TEST(ComptimeIf, GenericPatternSpecializationFmt) {
    const char *src =
        "func Test[T](arg: T): void {\n"
        "  comptime if (T extends own *?) { var a = move(arg); } else { var a = arg; }\n"
        "}\n";
    std::string once = fmt_source(src);
    EXPECT_NE(once.find("extends own *?"), std::string::npos) << once;
    EXPECT_EQ(fmt_source(once.c_str()), once);
}

TEST(ComptimeIf, GenericPatternMixedExpression) {
    clux_expect_ok(
        "func Test[T, N: comptime u64](arg: T): void {\n"
        "  comptime if (T extends own *? && N == (1 as u64)) {\n"
        "    var a = move(arg);\n"
        "  } else {\n"
        "    var a = arg;\n"
        "  }\n"
        "}\n"
        "func main(): i32 {\n"
        "  var p: own *i32 = new i32{ .i32{7} };\n"
        "  Test[own *i32, 1](move(p));\n"
        "  Test[i32, 2](5);\n"
        "  return 0;\n"
        "}\n",
        "generic_comptime_pattern_mixed");
}

TEST(GenericPack, ValuePackSpreadAndTuple) {
    clux_expect_ok(
        "func sum3(a: u64, b: u64, c: u64): u64 { return a + b + c; }\n"
        "func f[...N: comptime u64](): u64 {\n"
        "  var t = .{ N };\n"
        "  return t[0] + t[1] + t[2] + sum3(N) + sum3(...N);\n"
        "}\n"
        "func main(): i32 {\n"
        "  return (f[1u64, 2u64, 3u64]() - 18u64) as i32;\n"
        "}\n",
        "generic_value_pack_spread");
}

TEST(GenericType, RecursiveUnion) {
    clux_expect_ok(
        "union U[T] { A: T; B: own *U[T]; }\n"
        "func main(): i32 {\n"
        "  var u: own U[i32] = .U[i32]{ .A = 7 };\n"
        "  return u.A - 7;\n"
        "}\n",
        "generic_recursive_union");
}

TEST(GenericType, MutuallyRecursiveUnion) {
    clux_expect_ok(
        "union A[T] { x: T; b: own *B[T]; }\n"
        "union B[T] { y: T; a: own *A[T]; }\n"
        "func main(): i32 {\n"
        "  var a: own A[i32] = .A[i32]{ .x = 7 };\n"
        "  return a.x - 7;\n"
        "}\n",
        "generic_mutual_recursive_union");
}

TEST(GenericType, EnumGenericBaseType) {
    clux_expect_ok(
        "enum E[T]: T { A = 1, B = 2 }\n"
        "func main(): i32 {\n"
        "  var x: E[i32] = E[i32]::A;\n"
        "  return (x as i32) - 1;\n"
        "}\n",
        "generic_enum_base_type");
}

TEST(GenericReject, RecursiveAliasStillUnsupported) {
    clux_expect_reject(
        "type Rec[T] = own *Rec[T];\n"
        "func main(): i32 { var x: Rec[i32] = undefined; return 0; }\n",
        "recursive generic type 'Rec' is not supported yet",
        "generic_recursive_alias");
}

TEST(GenericType, ConditionalOwnershipAlias) {
    clux_expect_ok(
        "type Out[T] = T extends own *? ? fatal T : T;\n"
        "func f[T](x: T): Out[T] { return move(x); }\n"
        "func main(): i32 {\n"
        "  var p: own *i32 = new i32{ .i32{7} };\n"
        "  var q: own *i32 = f[own *i32](move(p));\n"
        "  var n: i32 = f[i32](5);\n"
        "  return q.* + n - 12;\n"
        "}\n",
        "generic_conditional_own_alias");
}

TEST(GenericType, ParenthesizedConditionalReturn) {
    clux_expect_ok(
        "func f[T](x: T): (T extends own *? ? fatal T : T) { return move(x); }\n"
        "func main(): i32 {\n"
        "  var p: own *i32 = new i32{ .i32{7} };\n"
        "  var q: own *i32 = f[own *i32](move(p));\n"
        "  var n: i32 = f[i32](5);\n"
        "  return q.* + n - 12;\n"
        "}\n",
        "generic_paren_conditional");
}

TEST(GenericType, ReflectionConditionalReturn) {
    clux_expect_ok(
        "func f[T](x: T): (T.ownership == \"own\" ? fatal T : T) {\n"
        "  return move(x);\n"
        "}\n"
        "func main(): i32 {\n"
        "  var p: own *i32 = new i32{ .i32{7} };\n"
        "  var q: own *i32 = f[own *i32](move(p));\n"
        "  var n: i32 = f[i32](5);\n"
        "  return q.* + n - 12;\n"
        "}\n",
        "generic_reflect_conditional");
}