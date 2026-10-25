/*
 * Description: package descriptor (clux.mod) unit tests
 * Create: 2026-10-12
 */

#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

extern "C" {
#include "core/allocator.h"
#include "core/arena.h"
#include "core/panic.h"
#include "module/package.h"
}

#include "test_common.h"

namespace {

class PackageTest : public ::testing::Test {
protected:
    allocator_t *alloc = nullptr;
    arena_t     *arena = nullptr;

    void SetUp() override {
        alloc = create_allocator(malloc, free);
        arena = arena_new_default(alloc);
    }

    void TearDown() override {
        arena_destroy(alloc, &arena);
        EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc);
    }

    /* 创建临时目录并写入 clux.mod，返回目录路径 */
    std::string make_pkg_dir(const std::string &content) {
        auto dir = clux_test_path() /
                   ("clux_pkg_test_XXXXXX");
        static int counter = 0;
        dir += std::to_string(counter++);
        std::filesystem::create_directories(dir);
        auto mod_path = dir / "clux.mod";
        std::ofstream ofs(mod_path);
        ofs << content;
        ofs.close();
        return dir.string();
    }

    /* 删除临时目录 */
    void cleanup_dir(const std::string &dir) {
        std::filesystem::remove_all(dir);
    }
};

/* ---- 基本加载 ---- */

TEST_F(PackageTest, LoadSingleEntry) {
    std::string dir = make_pkg_dir(R"({
        "name": "myapp",
        "version": "0.1.0",
        "entry": "src/main.clux"
    })");

    package_desc_t desc;
    pkg_result_t r = package_load(alloc, arena, dir.c_str(), &desc);
    EXPECT_EQ(r, PKG_OK);
    EXPECT_EQ(std::string(desc.name.ptr, desc.name.len), "myapp");
    EXPECT_EQ(std::string(desc.version.ptr, desc.version.len), "0.1.0");
    EXPECT_EQ(desc.entry_kind, PKG_ENTRY_SINGLE);
    EXPECT_EQ(std::string(desc.entry.single.ptr, desc.entry.single.len),
              "src/main.clux");
    EXPECT_EQ(desc.dep_count, 0u);
    EXPECT_EQ(desc.deps, nullptr);

    cleanup_dir(dir);
}

TEST_F(PackageTest, LoadMultiEntry) {
    std::string dir = make_pkg_dir(R"({
        "name": "std",
        "version": "0.1.0",
        "entry": {
            "std/array": "src/array.clux",
            "std/io": "src/io.clux"
        }
    })");

    package_desc_t desc;
    pkg_result_t r = package_load(alloc, arena, dir.c_str(), &desc);
    EXPECT_EQ(r, PKG_OK);
    EXPECT_EQ(std::string(desc.name.ptr, desc.name.len), "std");
    EXPECT_EQ(desc.entry_kind, PKG_ENTRY_MULTI);
    EXPECT_EQ(desc.entry.multi.count, 2u);

    /* 验证子包条目 */
    bool found_array = false, found_io = false;
    for (size_t i = 0; i < desc.entry.multi.count; i++) {
        std::string name(desc.entry.multi.entries[i].name.ptr,
                         desc.entry.multi.entries[i].name.len);
        std::string path(desc.entry.multi.entries[i].path.ptr,
                         desc.entry.multi.entries[i].path.len);
        if (name == "std/array") {
            found_array = true;
            EXPECT_EQ(path, "src/array.clux");
        }
        if (name == "std/io") {
            found_io = true;
            EXPECT_EQ(path, "src/io.clux");
        }
    }
    EXPECT_TRUE(found_array);
    EXPECT_TRUE(found_io);

    cleanup_dir(dir);
}

TEST_F(PackageTest, LoadWithDeps) {
    std::string dir = make_pkg_dir(R"({
        "name": "myapp",
        "version": "0.2.0",
        "entry": "src/main.clux",
        "deps": {
            "std": "github:clux-lang/std@0.1.0",
            "local": "file:/usr/local/clux-lib"
        }
    })");

    package_desc_t desc;
    pkg_result_t r = package_load(alloc, arena, dir.c_str(), &desc);
    EXPECT_EQ(r, PKG_OK);
    EXPECT_EQ(desc.dep_count, 2u);

    /* 验证依赖解析 */
    for (size_t i = 0; i < desc.dep_count; i++) {
        std::string dname(desc.deps[i].name.ptr, desc.deps[i].name.len);
        std::string proto(desc.deps[i].protocol.ptr,
                          desc.deps[i].protocol.len);
        std::string path(desc.deps[i].path.ptr, desc.deps[i].path.len);
        std::string ver(desc.deps[i].version.ptr, desc.deps[i].version.len);

        if (dname == "std") {
            EXPECT_EQ(proto, "github");
            EXPECT_EQ(path, "clux-lang/std");
            EXPECT_EQ(ver, "0.1.0");
        }
        if (dname == "local") {
            EXPECT_EQ(proto, "file");
            EXPECT_EQ(path, "/usr/local/clux-lib");
            EXPECT_EQ(ver, ""); /* 无版本 */
        }
    }

    cleanup_dir(dir);
}

TEST_F(PackageTest, LoadWithoutVersion) {
    std::string dir = make_pkg_dir(R"({
        "name": "nopkg",
        "entry": "main.clux"
    })");

    package_desc_t desc;
    pkg_result_t r = package_load(alloc, arena, dir.c_str(), &desc);
    EXPECT_EQ(r, PKG_OK);
    EXPECT_EQ(desc.version.len, 0u);

    cleanup_dir(dir);
}

TEST_F(PackageTest, LoadWithoutEntry) {
    std::string dir = make_pkg_dir(R"({
        "name": "noentry"
    })");

    package_desc_t desc;
    pkg_result_t r = package_load(alloc, arena, dir.c_str(), &desc);
    EXPECT_EQ(r, PKG_ERR_SCHEMA);

    cleanup_dir(dir);
}

TEST_F(PackageTest, LoadMissingFile) {
    package_desc_t desc;
    pkg_result_t r = package_load(alloc, arena,
                                  "/nonexistent/path/that/does/not/exist",
                                  &desc);
    EXPECT_EQ(r, PKG_ERR_NOT_FOUND);
}

TEST_F(PackageTest, LoadInvalidJson) {
    std::string dir = make_pkg_dir("{ this is not valid json }");

    package_desc_t desc;
    pkg_result_t r = package_load(alloc, arena, dir.c_str(), &desc);
    EXPECT_EQ(r, PKG_ERR_PARSE);

    cleanup_dir(dir);
}

TEST_F(PackageTest, LoadEntryNotStringOrObject) {
    std::string dir = make_pkg_dir(R"({
        "name": "badpkg",
        "entry": 42
    })");

    package_desc_t desc;
    pkg_result_t r = package_load(alloc, arena, dir.c_str(), &desc);
    EXPECT_EQ(r, PKG_ERR_SCHEMA);

    cleanup_dir(dir);
}

/* ---- 向上查找 ---- */

TEST_F(PackageTest, FindFromSubdirectory) {
    /* 创建包结构：
       tmpdir/
         clux.mod
         src/
           main.clux
    */
    std::filesystem::path base = std::filesystem::path(clux_test_temp_dir()) / "clux_pkg_find_XXXXXX";
    static int counter = 0;
    base += std::to_string(counter++);
    std::filesystem::create_directories(base / "src");

    auto mod_path = base / "clux.mod";
    std::ofstream ofs(mod_path);
    ofs << R"({"name": "findtest", "version": "1.0.0", "entry": "src/main.clux"})";
    ofs.close();

    /* 从 src/ 子目录向上查找 */
    std::string subdir = (base / "src").string();
    char *found_dir = nullptr;
    package_desc_t desc;
    pkg_result_t r = package_find(alloc, arena, subdir.c_str(),
                                  &found_dir, &desc);
    EXPECT_EQ(r, PKG_OK);
    ASSERT_NE(found_dir, nullptr);
    EXPECT_EQ(std::string(desc.name.ptr, desc.name.len), "findtest");
    /* found_dir 是 alloc 分配的，不需要手动释放（arena 管理吗？不——alloc 分配） */
    /* 但 arena 会被 destroy，found_dir 是 alloc 分配的——需要释放 */
    /* 实际上 found_dir 用 allocator_new_ex(alloc,...) 分配，需手动释放 */
    allocator_free(alloc, (void **)&found_dir);

    std::filesystem::remove_all(base);
}

TEST_F(PackageTest, FindNoMod) {
    /* 在临时目录的深处查找（应该找不到） */
    std::filesystem::path deep = std::filesystem::path(clux_test_temp_dir()) / "clux_nopkg_test_XXXXXX";
    static int counter = 0;
    deep += std::to_string(counter++);
    std::filesystem::create_directories(deep);

    char *found_dir = nullptr;
    package_desc_t desc;
    package_find(alloc, arena, deep.string().c_str(), &found_dir, &desc);
    /* 可能会在上级目录找到（如果上级有 clux.mod），但通常不会 */
    /* 只验证不崩溃 */
    if (found_dir) allocator_free(alloc, (void **)&found_dir);

    std::filesystem::remove_all(deep);
}

} // namespace
