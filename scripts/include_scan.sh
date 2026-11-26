#!/usr/bin/env bash
# clux include 扫描（sema-design §5-1）
#
# compiler 源文件只允许 parser/、core/、diag/ 头（本地相对 include 与
# 系统 include 除外）。src/compiler 尚未落地时空转放行（退出码 0）。
# 同一防线亦常驻于 tests/test_purity.cpp（gtest），本脚本供 CI/本地单独调用。
#
# 用法：
#   ./scripts/include_scan.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="$ROOT/src/compiler"

if [ ! -d "$TARGET" ]; then
  echo "include-scan: src/compiler 不存在，空转放行"
  exit 0
fi

# 提取全部带引号 #include 的路径部分，白名单外（既非 parser//core//diag/
# 前缀、又含 '/' 的路径）即违规。系统 include <...> 不在提取范围。
violations="$(
  find "$TARGET" -type f \( -name '*.c' -o -name '*.h' \) -exec \
    grep -nE '^[[:space:]]*#[[:space:]]*include[[:space:]]*"[^"]+"' {} + |
    sed -E 's/.*#[[:space:]]*include[[:space:]]*"([^"]+)".*/\1/' |
    grep -vE '^(parser/|core/|diag/|[^/]+)$' || true
)"

if [ -n "$violations" ]; then
  echo "include-scan: 发现白名单（parser//core//diag/）外的 include：" >&2
  printf '  %s\n' $violations >&2
  exit 1
fi

echo "include-scan: PASS"
