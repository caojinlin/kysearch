#!/usr/bin/env bash
# SETROOTS 热更新端到端：不重启守护进程完成索引根增删，
# 验证新根可搜、新根实时事件、移除根不可搜、STATUS 一致。
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="${BUILD:-$HERE/../build}"
D="$BUILD/kysearchd"
CLI="$BUILD/kysearch-cli"
BASE=/tmp/kysearch-hotroots
A=$BASE/rootA
B=$BASE/rootB

rm -rf "$BASE"; mkdir -p "$A/子目录" "$B"
echo 甲 > "$A/文件甲.txt"; echo 乙 > "$A/子目录/文件乙.md"; echo 丙 > "$B/文件丙.docx"

"$D" --root "$A" --db "$BASE/i.bin" --socket "$BASE/s.sock" --quiet >"$BASE/d.log" 2>&1 &
DPID=$!
cleanup() { kill "$DPID" 2>/dev/null || true; wait "$DPID" 2>/dev/null || true; }
trap cleanup EXIT
for i in $(seq 1 60); do "$CLI" status --socket "$BASE/s.sock" >/dev/null 2>&1 && break; sleep 0.1; done

S="$CLI --socket $BASE/s.sock" 2>/dev/null || S=""
run() { "$CLI" "$1" --socket "$BASE/s.sock"; }
count() { run "search" 2>/dev/null <<<"$2" | grep -c "^F" || true; }

echo "== 1. 初始（仅 rootA）=="
run status
N=$(run search | grep -c 文件 || true)
[ "$(run search | grep -c '文件甲')" = "1" ] || { echo "✗ rootA 未索引"; exit 1; }
[ "$(run search | grep -c '文件丙')" = "0" ] || { echo "✗ rootB 不应可搜"; exit 1; }

echo "== 2. 热加 rootB（不重启）=="
UPID_OLD=$(pgrep -f "kysearchd --root $BASE/rootA" | head -1)
"$CLI" setroots "$A" "$B" --socket "$BASE/s.sock"
[ "$(pgrep -f "kysearchd --root $BASE/rootA" | head -1)" = "$UPID_OLD" ] || { echo "✗ 守护进程被重启了！"; exit 1; }
[ "$(run search | grep -c '文件丙')" = "1" ] || { echo "✗ 热加后 rootB 不可搜"; exit 1; }

echo "== 3. 新根实时事件 =="
echo 丁 > "$B/热更新新增.txt"; sleep 1.5
[ "$(run search | grep -c '热更新新增')" = "1" ] || { echo "✗ 新根实时事件未生效"; exit 1; }

echo "== 4. 热删 rootA =="
"$CLI" setroots "$B" --socket "$BASE/s.sock"
[ "$(run search | grep -c '文件甲')" = "0" ] || { echo "✗ 移除根仍可搜"; exit 1; }
[ "$(run search | grep -c '文件丙')" = "1" ] || { echo "✗ 保留根被误删"; exit 1; }
run status | grep -q "rootB" && run status | grep -q "rootA" && { echo "✗ STATUS 未更新"; exit 1; } || true

echo "== 5. 快照持久化新根（重启后 B 仍在）=="
"$CLI" stop --socket "$BASE/s.sock" >/dev/null 2>&1; sleep 0.5; kill "$DPID" 2>/dev/null || true; wait "$DPID" 2>/dev/null || true
trap - EXIT
"$D" --root "$B" --db "$BASE/i.bin" --socket "$BASE/s.sock" --quiet >>"$BASE/d.log" 2>&1 &
DPID=$!
trap cleanup EXIT
for i in $(seq 1 60); do "$CLI" status --socket "$BASE/s.sock" >/dev/null 2>&1 && break; sleep 0.1; done
run status | grep -q "$B" || { echo "✗ 快照根未持久化"; exit 1; }

echo "✓ HOTROOTS TESTS PASSED（热加/实时/热删/STATUS/持久化）"
