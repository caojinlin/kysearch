#!/usr/bin/env bash
# 守护进程端到端测试：启动 kysearchd → 查询 → 实时增删改名/目录树验证 → RESCAN → 优雅退出
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="${BUILD:-$HERE/../build}"
D="$BUILD/kysearchd"
CLI="$BUILD/kysearch-cli"
TREE=/tmp/kysearch-daemon/tree
BASE=/tmp/kysearch-daemon
DB=$BASE/index.bin
SOCK=$BASE/daemon.sock
LOG=$BASE/daemon.log

fail() { echo "✗ $1"; exit 1; }
pass() { echo "✓ $1"; }

rm -rf "$BASE"; mkdir -p "$BASE"
python3 "$HERE/gen_tree.py" "$TREE" > "$BASE/expect.env"
source "$BASE/expect.env"

"$D" --root "$TREE" --db "$DB" --socket "$SOCK" --rescan-interval 3600 \
     --content-roots "$TREE" > "$LOG" 2>&1 &
DPID=$!
cleanup() { kill "$DPID" 2>/dev/null || true; wait "$DPID" 2>/dev/null || true; }
trap cleanup EXIT

# 等待就绪
for i in $(seq 1 60); do
  [ -S "$SOCK" ] && "$CLI" status --socket "$SOCK" >/dev/null 2>&1 && break
  sleep 0.1
done
[ -S "$SOCK" ] || fail "守护进程未就绪（日志：$(tail -3 "$LOG" 2>/dev/null)）"

echo "== 1. STATUS 与初始查询"
STATUS=$("$CLI" status --socket "$SOCK")
echo "  $STATUS"
grep -qE 'backend=(fanotify|inotify|rescan)' <<<"$STATUS" || fail "STATUS 异常"
BACKEND=$(grep -oE 'backend=[a-z]+' <<<"$STATUS" | cut -d= -f2)
pass "监控后端 = $BACKEND"

rcount() { "$CLI" search --socket "$SOCK" "$@" 2>/dev/null | awk '!/^#/{n++} END{print n+0}'; }
assert_eq() {
  local name=$1 expect=$2; shift 2
  local got; got=$(rcount "$@")
  [ "$got" -eq "$expect" ] || fail "$name: 期望 $expect 实际 $got ($*)"
  pass "$name = $got"
}

assert_eq "初始查询 手机" "$EXPECT_PHONE" 手机
assert_eq "过滤器 ext:docx" 5000 ext:docx
assert_eq "正则 ^IMG_2025" 5000 --regex '^IMG_2025'

echo "== 2. 实时性验证（$BACKEND 后端 + 200ms 去抖）"
echo "验收标准内容" > "$TREE/实时新增文件.txt"
sleep 1.5
assert_eq "新建文件立即可搜" 1 实时新增文件

mkdir -p "$TREE/新目录/子目录"
echo y > "$TREE/新目录/子目录/嵌套文档.md"
sleep 2.5  # 内容巡检期间 IO 竞争，放宽等待
assert_eq "新建目录整树可见(文件)" 1 嵌套文档
assert_eq "新建目录整树可见(目录)" 2 --path 新目录/子目录

mv "$TREE/手机照片/手机00000.jpg" "$TREE/手机照片/改名00000.jpg"
sleep 1.5
assert_eq "改名后新名可搜" 1 改名00000
assert_eq "改名后旧名不可搜" 0 手机00000
assert_eq "改名后计数正确" $((EXPECT_PHONE - 1)) 手机

echo "更多内容" >> "$TREE/实时新增文件.txt"
sleep 1.5
assert_eq "修改文件仍可搜" 1 实时新增文件

rm "$TREE/实时新增文件.txt"
sleep 1.5
assert_eq "删除后不可搜" 0 实时新增文件

rm -rf "$TREE/新目录"
sleep 1.5
assert_eq "删除目录树后子文件不可搜" 0 嵌套文档

echo "== 2b. 全文内容搜索（content: 关键词）"
echo "本次验收标准为麒麟系统适配版本，测试里程碑全部达成。" > "$TREE/会议纪要_001.md"
sleep 2.5  # 初始内容巡检 + 事件入队
assert_eq "内容关键词检索" 1 content:验收标准
echo "补充条目：性能基线达标。" >> "$TREE/会议纪要_001.md"
sleep 1.5
assert_eq "内容修改后新词可检" 1 content:补充条目
assert_eq "内容+文件名联合" 1 会议纪要 content:验收标准
rm "$TREE/会议纪要_001.md"
sleep 1.5
assert_eq "内容删除后不可检" 0 content:验收标准

echo "== 3. RESCAN 对账"
python3 - "$SOCK" <<'EOF'
import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
s.sendall(b"RESCAN\n"); s.shutdown(socket.SHUT_WR)
print("  " + s.recv(4096).decode().strip())
EOF
sleep 2
assert_eq "对账后查询一致" $((EXPECT_PHONE - 1)) 手机

echo "== 4. 优雅退出（STOP 落盘）"
"$CLI" stop --socket "$SOCK" >/dev/null
for i in $(seq 1 40); do kill -0 "$DPID" 2>/dev/null || break; sleep 0.1; done
kill -0 "$DPID" 2>/dev/null && fail "守护进程未在 4s 内退出"
wait "$DPID" || RC=$?
trap - EXIT
grep -q "快照" "$LOG" && pass "退出时保存了快照（$(grep -c 快照 "$LOG") 次）"
[ -f "$DB" ] || fail "快照文件不存在"

echo ""
echo "ALL DAEMON TESTS PASSED（后端: $BACKEND）"
