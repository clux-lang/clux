/**
 * File: bcode_graph_text_test.cpp
 * Description: 模块图文本形态（`.cxs`）解析/渲染 —— `bcode_graph_asm_parse` /
 *              `bcode_graph_disasm_mem`。重点覆盖 M6 起才有的 `externlib` 行
 *              （手写汇编声明 FFI 库表的唯一途径）与**失败路径不泄漏**。
 *
 * 背景：`examples/asm/` 下的 .cxs 样本一度全部"能汇编、不能运行"（printf 自 M6
 * 起不再是内建，样本却仍写 `push "printf"; call N`），且 `.cxs → 图` 的失败路径
 * 会漏掉已部分构建的模块映像（`clux.vm.graph.mods` / `clux.vm.graph.path`）。
 * 这条路径此前完全没有测试覆盖，两个问题都没被发现。
 */

#include <gtest/gtest.h>

#include <cstring>
#include <string>

extern "C" {
#include "core/allocator.h"
#include "vm/bcode_graph.h"
}

#include "test_common.h"

namespace {

const char *kFfiSample =
    "externlib \"*\" as libc\n"
    "push_string \"hi\\n\"\n"
    "extern_call \"libc::printf\" 1 65792\n"
    "pop\n"
    "halt\n";

} // namespace

TEST(BcodeGraphText, ParsesExternlibAndCode) {
  allocator_t *a = create_allocator(malloc, free);
  bcode_graph_t g;
  ASSERT_EQ(bcode_graph_asm_parse(a, kFfiSample, std::strlen(kFfiSample), &g), 0);

  ASSERT_EQ(g.count, 1u);
  EXPECT_EQ(g.entry_index, 0u);
  bcode_module_image_t *m = &g.modules[0];
  EXPECT_STREQ(m->import_path, ""); /* 裸段 = 入口模块 */
  ASSERT_EQ(m->ffi_count, 1u);
  EXPECT_STREQ(m->ffi_aliases[0], "libc");
  EXPECT_STREQ(m->ffi_paths[0], "*"); /* "*" = 进程自身符号 */
  ASSERT_NE(m->bc, nullptr);

  bcode_graph_free(&g);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(BcodeGraphText, DisasmKeepsExternlibLine) {
  /* 往返：解析 → 渲染文本 → 再解析。`externlib` 行必须原样写回（否则
     `bc disasm` 产物会丢掉库表，`bc asm` 回不去）。 */
  allocator_t *a = create_allocator(malloc, free);
  bcode_graph_t g;
  ASSERT_EQ(bcode_graph_asm_parse(a, kFfiSample, std::strlen(kFfiSample), &g), 0);

  size_t len = 0;
  char *text = bcode_graph_disasm_mem(a, &g, &len);
  ASSERT_NE(text, nullptr);
  const std::string rendered(text, len);
  EXPECT_NE(rendered.find("externlib \"*\" as libc"), std::string::npos) << rendered;
  EXPECT_NE(rendered.find("EXTERN_CALL \"libc::printf\" 1 65792"), std::string::npos)
      << rendered;

  bcode_graph_t g2;
  ASSERT_EQ(bcode_graph_asm_parse(a, rendered.data(), rendered.size(), &g2), 0);
  ASSERT_EQ(g2.count, 1u);
  ASSERT_EQ(g2.modules[0].ffi_count, 1u);
  EXPECT_STREQ(g2.modules[0].ffi_aliases[0], "libc");

  bcode_graph_free(&g2);
  bcode_graph_free(&g);
  allocator_free(a, (void **)&text);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(BcodeGraphText, AssembleFailureLeaksNothing) {
  /* 回归：`asm: module N failed to assemble` 时此前会漏掉
     `clux.vm.graph.mods`（模块映像表）与循环内新建的 `clux.vm.graph.path`
     （入口模块路径）——`clux bc asm` 对坏文件报两条泄漏。 */
  const char *bad = "externlib \"*\" as libc\n"
                    "bogus_mnemonic_xyz 1\n";
  allocator_t *a = create_allocator(malloc, free);
  bcode_graph_t g;
  EXPECT_NE(bcode_graph_asm_parse(a, bad, std::strlen(bad), &g), 0);
  bcode_graph_free(&g); /* 失败后图应为空图（可安全再 free） */
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(BcodeGraphText, MalformedExternlibRejected) {
  /* `externlib` 行必须带 `as <别名>`：坏行报错且不留分配 */
  const char *bad = "externlib \"*\"\n"
                    "halt\n";
  allocator_t *a = create_allocator(malloc, free);
  bcode_graph_t g;
  EXPECT_NE(bcode_graph_asm_parse(a, bad, std::strlen(bad), &g), 0);
  bcode_graph_free(&g);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}
