#!/usr/bin/env bash
# aarch64（飞腾/鲲鹏）交叉编译 + QEMU 用户态实测
# 产物：静态链接、无外部依赖的 kysearch-cli / kysearch-test / kysearchd
# 可选依赖（RE2/SQLite/Qt）在交叉模式禁用——核心引擎不依赖它们
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build-arm64"

cmake -S "$ROOT" -B "$OUT" \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
  -DCMAKE_FIND_ROOT_PATH=/usr/aarch64-linux-gnu \
  -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
  -DKYSEARCH_MINIMAL=ON \
  -DCMAKE_EXE_LINKER_FLAGS="-static" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$OUT" -j"$(nproc)" --target kysearch-cli kysearch-test kysearchd

file "$OUT/kysearch-cli" | grep -q 'ARM aarch64' || { echo "✗ 非 aarch64 产物"; exit 1; }
echo "== QEMU 运行单元测试 =="
qemu-aarch64 "$OUT/kysearch-test"

echo "== QEMU 运行 CLI 实测（扫描 + 中文/拼音/过滤查询） =="
TREE=/tmp/kysearch-arm64-tree
rm -rf "$TREE"; mkdir -p "$TREE/文档"
for i in $(seq 1 200); do echo "麒麟适配测试内容 $i" > "$TREE/文档/验收报告_$i.txt"; done
qemu-aarch64 "$OUT/kysearch-cli" scan "$TREE" --db /tmp/kysearch-arm64.bin --threads 2
Q() { qemu-aarch64 "$OUT/kysearch-cli" search --db /tmp/kysearch-arm64.bin "$@"; }
N=$(Q 验收报告 | awk '!/^#/{n++} END{print n+0}')
[ "$N" -eq 200 ] || { echo "✗ 中文名查询: $N"; exit 1; }
N=$(Q yanshou | awk '!/^#/{n++} END{print n+0}')
[ "$N" -eq 200 ] || { echo "✗ 拼音查询: $N"; exit 1; }
N=$(Q ext:txt type:file | awk '!/^#/{n++} END{print n+0}')
[ "$N" -eq 200 ] || { echo "✗ 过滤器查询: $N"; exit 1; }
Q 麒麟适配 --limit 2 | head -2
echo "AARCH64 CROSS + QEMU TESTS PASSED"
rm -rf "$TREE" /tmp/kysearch-arm64.bin
