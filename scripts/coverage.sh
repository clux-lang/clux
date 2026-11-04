#!/usr/bin/env bash
# clux 覆盖率报告脚本（LLVM source-based coverage）
#
# 用法：
#   ./scripts/coverage.sh              # 构建 + 跑测试 + 出报告
#   ./scripts/coverage.sh --report     # 只出报告（复用已有 build-cov）
#
# 依赖：clang（含 -fprofile-instr-generate）、llvm-profdata、llvm-cov。
# Windows 下需把 LLVM 的 bin 目录加入 PATH，或设置 LLVM_BIN 环境变量。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build-cov"
LLVM_BIN="${LLVM_BIN:-}"

find_tool() {
  local name="$1"
  if [ -n "$LLVM_BIN" ] && [ -x "$LLVM_BIN/$name" ]; then
    echo "$LLVM_BIN/$name"
  elif command -v "$name" >/dev/null 2>&1; then
    command -v "$name"
  else
    echo ""
  fi
}

PROFDATA="$(find_tool llvm-profdata)"
LLVMCOV="$(find_tool llvm-cov)"
if [ -z "$PROFDATA" ] || [ -z "$LLVMCOV" ]; then
  echo "error: llvm-profdata / llvm-cov not found. Set LLVM_BIN or add LLVM bin to PATH." >&2
  exit 1
fi

if [ "${1:-}" != "--report" ]; then
  cmake -B "$BUILD" -DCLUX_COVERAGE=ON >/dev/null
  cmake --build "$BUILD" --target clux_test -j "$(nproc 2>/dev/null || echo 8)"
  ( cd "$BUILD" && rm -f ./*.profraw \
      && LLVM_PROFILE_FILE="cov-%p.profraw" ./clux_test )
fi

cd "$BUILD"
rm -f cov.profdata
"$PROFDATA" merge -sparse ./*.profraw -o cov.profdata
"$LLVMCOV" report ./clux_test* \
  -instr-profile=cov.profdata \
  --ignore-filename-regex='(third_party|tests|_deps)'
