#!/usr/bin/env bash
# 对账重扫 × 实时变更 竞态压测：
#   守护进程以 1s 间隔高频对账（swap 索引），同时持续创建/删除文件，
#   结束后全量校验——任何在「扫描窗口内」发生的变更丢失即失败。
# 复现历史缺陷：swap 用扫描开始时的快照覆盖索引，扫描期间实时 upsert
# 的条目被抹掉（修复：monitor 回放日志 recent_ 幂等重放）。
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="${BUILD:-$HERE/../build}"
D="$BUILD/kysearchd"
CLI="$BUILD/kysearch-cli"
BASE=/tmp/kysearch-race
TREE=$BASE/tree

rm -rf "$BASE"; mkdir -p "$TREE"
python3 - "$TREE" <<'EOF'  # 3 万文件底座，拉长单次对账扫描窗口
import os, sys
root = sys.argv[1]
for i in range(60):
    d = f"{root}/d{i:03d}"
    os.makedirs(d, exist_ok=True)
    for j in range(500):
        open(f"{d}/f{j:04d}.txt", "w").write("x")
EOF
BASE_N=$((60 * 500 + 60))

"$D" --root "$TREE" --db "$BASE/i.bin" --socket "$BASE/d.sock" \
     --rescan-interval 1 --rescan-initial-delay 1 --quiet >"$BASE/d.log" 2>&1 &
DPID=$!
cleanup() { kill "$DPID" 2>/dev/null || true; wait "$DPID" 2>/dev/null || true; }
trap cleanup EXIT
for i in $(seq 1 100); do
  [ -S "$BASE/d.sock" ] && "$CLI" status --socket "$BASE/d.sock" >/dev/null 2>&1 && break
  sleep 0.1
done

# 风暴：8 轮 ×（创建 25 + 删除上轮一半），每轮间隔 0.3s，总与 ~10 次对账重叠
for round in $(seq 1 8); do
  for j in $(seq 1 25); do
    echo race > "$TREE/r${round}_${j}.txt"
  done
  if [ "$round" -gt 1 ]; then
    for j in $(seq 1 12); do rm -f "$TREE/r$((round-1))_$j.txt"; done
  fi
  sleep 0.3
done
sleep 3  # 末轮对账 + 回放落地

# 校验：存活文件 = 每轮 25，减去后续删除的 12×7 轮（首轮全保留？首轮被第2轮删12…）
ALIVE=$((25 * 8 - 12 * 7))
GOT=$("$CLI" search --socket "$BASE/d.sock" --regex '^r[0-9]+_[0-9]+\.txt$' 2>/dev/null | grep -c '^F' || true)
GOTD=$("$CLI" search --socket "$BASE/d.sock" --regex '^r[0-9]+_[0-9]+\.txt$' 2>/dev/null | grep -c '^D' || true)
echo "存活变更文件: $GOT / 期望 $ALIVE（目录误入: $GOTD）"
TOTAL=$("$CLI" status --socket "$BASE/d.sock" | grep -oE 'entries=[0-9]+' | cut -d= -f2)
EXPECT_TOTAL=$((BASE_N + ALIVE))
echo "总条目: $TOTAL / 期望 $EXPECT_TOTAL"
FAIL=0
[ "$GOT" -eq "$ALIVE" ] || { echo "✗ 竞态复现：变更文件丢失 $((ALIVE - GOT)) 个"; FAIL=1; }
[ "$TOTAL" -eq "$EXPECT_TOTAL" ] || { echo "✗ 条目漂移: 差 $((TOTAL - EXPECT_TOTAL))"; FAIL=1; }
if [ "$FAIL" -eq 0 ]; then echo "✓ RESCAN RACE TEST PASSED（8 轮风暴 × ~10 次对账零丢失）"; else
  echo "（对照：底层底座 $BASE_N 存活变更 $ALIVE）"; exit 1; fi
