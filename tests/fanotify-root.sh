#!/usr/bin/env bash
# fanotify（L1）root 冒烟测试：验证文件系统级监控 + 句柄路径解析真实可用
# 需要免密 sudo（本测试服务器满足）；生产环境对应 systemd 系统服务场景
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="${BUILD:-$HERE/../build}"
D="$BUILD/kysearchd"
CLI="$BUILD/kysearch-cli"
BASE=/tmp/kysearch-fan
TREE=$BASE/tree

fail() { echo "✗ $1"; sudo rm -rf "$BASE"; exit 1; }
pass() { echo "✓ $1"; }

rm -rf "$BASE"; mkdir -p "$TREE"
for i in $(seq 1 50); do echo "初始数据$i" > "$TREE/初始文件$i.txt"; done

sudo "$D" --root "$TREE" --db "$BASE/index.bin" --socket "$BASE/daemon.sock" \
  --level fanotify --rescan-interval 3600 > "$BASE/daemon.log" 2>&1 &
DPID=$!
cleanup() { sudo kill "$DPID" 2>/dev/null || true; sudo rm -rf "$BASE"; }
trap cleanup EXIT

for i in $(seq 1 80); do
  [ -S "$BASE/daemon.sock" ] && "$CLI" status --socket "$BASE/daemon.sock" >/dev/null 2>&1 && break
  sleep 0.1
done
[ -S "$BASE/daemon.sock" ] || fail "守护进程未就绪（$(tail -3 "$BASE/daemon.log")）"

STATUS=$("$CLI" status --socket "$BASE/daemon.sock")
echo "  $STATUS"
grep -q 'backend=fanotify' <<<"$STATUS" || fail "后端不是 fanotify：$STATUS"
pass "监控后端 = fanotify（文件系统级）"

sudo sh -c "echo x > '$TREE/fanotify新建.txt'"
sleep 1.5
N=$("$CLI" search --socket "$BASE/daemon.sock" fanotify新建 2>/dev/null | awk '!/^#/{n++} END{print n+0}')
[ "$N" -eq 1 ] || fail "新建文件实时可搜: $N"
pass "新建文件实时可搜 = 1"

sudo sh -c "mv '$TREE/初始文件1.txt' '$TREE/fanotify改名.txt'"
sleep 1.5
N=$("$CLI" search --socket "$BASE/daemon.sock" fanotify改名 2>/dev/null | awk '!/^#/{n++} END{print n+0}')
[ "$N" -eq 1 ] || fail "改名实时可搜: $N"
pass "改名实时可搜 = 1"

sudo sh -c "rm '$TREE/fanotify新建.txt'"
sleep 1.5
N=$("$CLI" search --socket "$BASE/daemon.sock" fanotify新建 2>/dev/null | awk '!/^#/{n++} END{print n+0}')
[ "$N" -eq 0 ] || fail "删除实时生效: $N"
pass "删除实时生效 = 0"

"$CLI" stop --socket "$BASE/daemon.sock" >/dev/null
sleep 1
pass "fanotify L1 冒烟测试全部通过"
trap - EXIT
sudo rm -rf "$BASE"
echo "FANOTIFY ROOT SMOKE TEST PASSED"
