/* Phase 6（§5 验证标准）：纯度回归 + include 扫描 + 序列化往返 +
   语言子集断言 + 原 AST 不可变。全部走常驻测试（clux_test），CI 与
   本地共用同一防线。 */
#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/json.h"
#include "core/location.h"
#include "core/program.h"
#include "core/string.h"
#include "core/vec.h"
#include "engine/context.h"
#include "parser/node.h"
#include "parser/node_binary.h"
#include "parser/node_call.h"
#include "parser/node_construct.h"
#include "parser/node_program.h"
#include "parser/node_stmt_struct.h"
#include "parser/node_stmt_var.h"
#include "parser/parser.h"
#include "parser/program.h"
#include "sema/sema.h"
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string.h>
#include <vector>

static node_t *parse_clux(allocator_t *a, parser_t **p, const char *src) {
  *p = create_parser(a, "test.clux", src);
  return parser_read_program(a, *p);
}

static void dispose_programs(vec_t *programs) {
  if (!programs) {
    return;
  }
  for (size_t idx = 0; idx < vec_get_size(programs); idx++) {
    program_t *program = (program_t *)vec_get(programs, idx);
    delete_program(&program);
  }
  delete_vec(&programs);
}

static char *node_text(allocator_t *a, node_t *node) {
  return node ? location_get(node_get_location(node), a) : NULL;
}

/* 全树 JSON：writer 需要 source 还原 span/text 原文。 */
static std::string
json_snapshot(allocator_t *a, node_t *program_node, const char *source) {
  json_writer_t *writer = create_json_writer(a, source);
  program_node_to_json(writer, program_node);
  string_t *json = json_writer_release(writer);
  delete_json_writer(&writer);
  std::string snapshot(string_get_data(json), string_get_size(json));
  delete_string(&json);
  return snapshot;
}

/* 覆盖全部已落地变换的源码：提升/实例化/常量折叠/推断显式化/defer/
   comptime if/两类 foreach/匿名构造/struct 成员提升 + member_call。 */
static const char *kRichSource =
    "comptime var LIMIT = 3;\n"
    "type Pair[T] = [2]T;\n"
    "var p: Pair[i32];\n"
    "struct Box { v: i32; func get(self: *Box): i32 { return self.v; } }\n"
    "var r = .Box{ .v = 7 }.get();\n"
    "var anon = .{ .a = 1 };\n"
    "var inf = 42;\n"
    "func f(items: [2]i32): void {\n"
    "  defer { var t = 1; }\n"
    "  comptime if (true) { var a = 1; }\n"
    "  comptime foreach (x of <1, 2>) { var y = x; }\n"
    "  foreach (q of items) { var z = q; }\n"
    "  var use = LIMIT;\n"
    "}\n";

/* §5-5 原 AST 不可变：sema 前后原树的 to_json 完全一致
   （walk 只读，新建节点只走渲染器/克隆器两条通道）。 */
TEST(SemaPurityTest, OriginalAstJsonIsUnchangedBySema) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a, &p, kRichSource);
  ASSERT_NE(ast, nullptr);
  std::string before = json_snapshot(a, ast, kRichSource);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  std::string after = json_snapshot(a, ast, kRichSource);
  EXPECT_EQ(before, after);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* §5-4 语言子集断言：emit 产物不含 STMT_DEFER / STMT_COMPTIME_IF /
   STMT_COMPTIME_FOREACH / 泛型声明节点；STMT_VAR 与 CONSTRUCT 的 TYPE
   槽非 NULL；call 的 callee 不再是 "." member 访问形态。 */
static void assert_language_subset(allocator_t *a, node_t *node) {
  if (!node) {
    return;
  }
  node_kind_t kind = node_get_kind(node);
  EXPECT_NE(kind, CLUX_NODE_STMT_DEFER);
  EXPECT_NE(kind, CLUX_NODE_STMT_COMPTIME_IF);
  EXPECT_NE(kind, CLUX_NODE_STMT_COMPTIME_FOREACH);
  EXPECT_NE(kind, CLUX_NODE_GENERIC_PARAMS);
  EXPECT_NE(kind, CLUX_NODE_GENERIC_PARAM);
  if (kind == CLUX_NODE_STMT_VAR) {
    /* TYPE 槽非 NULL 断言限定 §2.8 已覆盖的推断形态：字面量值
       （bool/数值）与构造值（具名构造/已具名化的匿名构造）。
       调用/标识符等其余形态依赖类型检查完成（sema-design §2.8
       依赖声明），届时此断言收紧为无条件。 */
    node_t *value = stmt_var_node_get_value(node);
    node_kind_t value_kind = value ? node_get_kind(value) : CLUX_NODE_PROGRAM;
    int covered = value_kind == CLUX_NODE_LITERAL_BOOLEAN ||
                  value_kind == CLUX_NODE_LITERAL_NUMERIC ||
                  value_kind == CLUX_NODE_EXPR_CONSTRUCT;
    if (covered && !stmt_var_node_get_type(node)) {
      char *name = node_text(a, stmt_var_node_get_name(node));
      ADD_FAILURE() << "STMT_VAR 无 TYPE 槽: " << (name ? name : "?");
      allocator_free(a, (void **)&name);
    }
  }
  if (kind == CLUX_NODE_EXPR_CONSTRUCT) {
    EXPECT_NE(construct_node_get_type(node), nullptr);
  }
  if (kind == CLUX_NODE_EXPR_CALL) {
    node_t *callee = call_node_get_callee(node);
    if (callee && node_get_kind(callee) == CLUX_NODE_EXPR_BINARY) {
      char *symbol = node_text(a, binary_node_get_symbol(callee));
      EXPECT_TRUE(symbol == NULL || strcmp(symbol, ".") != 0);
      allocator_free(a, (void **)&symbol);
    }
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; children && idx < vec_get_size(children); idx++) {
    assert_language_subset(a, (node_t *)vec_get(children, idx));
  }
}

TEST(SemaPurityTest, EmittedAstStaysInLanguageSubset) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a, &p, kRichSource);
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  ASSERT_EQ(vec_get_size(programs), 1u);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  ASSERT_NE(decls, nullptr);
  /* 逐变换改写后整体仍落在编译器可吃的语言子集内。 */
  assert_language_subset(a, decls);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* §5-2 纯度回归 + §5-3 序列化往返：sema 输出后销毁全部 sema/engine
   状态，产物仍可全量遍历与序列化（ASAN 抓 use-after-free）；JSON
   不含地址（0x 形态），第一版豁免 JSON 重建等价 AST。 */
TEST(SemaPurityTest, EmittedProgramsSurviveEngineTeardownAndSerializeCleanly) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a, &p, kRichSource);
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  ASSERT_EQ(vec_get_size(programs), 1u);
  /* 引擎侧全灭：sema/parser/原 AST/context 依序销毁，产物只剩
     out allocator 一条所有权链。 */
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  /* 此时仍能读：模块名非空。 */
  program_t *program = (program_t *)vec_get(programs, 0);
  EXPECT_STREQ(program_get_module_name(program), "main");
  node_t *decls = program_get_decls(program);
  ASSERT_NE(decls, nullptr);
  EXPECT_GT(program_node_get_stmt_count(decls), 0u);
  /* 全树序列化：读遍每个 location/text，JSON 内不得出现地址形态。 */
  std::string json = json_snapshot(a, decls, kRichSource);
  EXPECT_NE(json.find("\"kind\""), std::string::npos);
  EXPECT_EQ(json.find("0x"), std::string::npos);
  dispose_programs(programs);
  delete_allocator(&a);
}

/* §5-1 include 扫描：compiler 源文件只允许 parser/、core/、diag/ 头
   （本地相对 include 与系统 include 除外）。src/compiler 尚未落地时
   防线空转放行，目录落地后断言自动生效。 */
static bool include_path_is_allowed(const std::string &path) {
  return path.rfind("parser/", 0) == 0 || path.rfind("core/", 0) == 0 ||
         path.rfind("diag/", 0) == 0 || path.find('/') == std::string::npos;
}

TEST(SemaPurityTest, CompilerIncludesStayWithinAllowedHeaders) {
  namespace fs = std::filesystem;
  const std::string root = CLUX_SOURCE_DIR "/src/compiler";
  if (!fs::exists(root)) {
    SUCCEED() << "src/compiler 尚未落地，include 扫描空转放行";
    return;
  }
  const std::regex quoted(R"re(#\s*include\s*"([^"]+)")re");
  std::vector<std::string> violations;
  for (const auto &entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string path = entry.path().string();
    if (path.size() < 2 || path.substr(path.size() - 2) != ".c") {
      if (path.size() < 2 || path.substr(path.size() - 2) != ".h") {
        continue;
      }
    }
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
      std::smatch match;
      if (std::regex_search(line, match, quoted) &&
          !include_path_is_allowed(match[1].str())) {
        std::ostringstream violation;
        violation << path << ": #include \"" << match[1].str() << "\"";
        violations.push_back(violation.str());
      }
    }
  }
  EXPECT_TRUE(violations.empty())
      << "compiler 源文件出现白名单（parser//core//diag/）之外的 include：\n"
      << [&] {
           std::ostringstream joined;
           for (const auto &violation : violations) {
             joined << "  " << violation << "\n";
           }
           return joined.str();
         }();
}
