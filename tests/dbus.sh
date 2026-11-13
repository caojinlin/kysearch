#!/usr/bin/env bash
# D-Bus 服务接口测试：dbus-run-session 内启动 kysearchd --dbus，
# 用 dbus-send 调用 org.kysearch.Daemon.Search / Ping 并断言结果
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="${BUILD:-$HERE/../build}"
D="$BUILD/kysearchd"
CLI="$BUILD/kysearch-cli"
TREE=/tmp/kysearch-dbus/tree
BASE=/tmp/kysearch-dbus

fail() { echo "✗ $1"; pkill -x kysearchd 2>/dev/null || true; rm -rf "$BASE"; exit 1; }
pass() { echo "✓ $1"; }

rm -rf "$BASE"; mkdir -p "$TREE/手机照片"
for i in $(seq 1 30); do echo "内容$i" > "$TREE/手机照片/手机$i.jpg"; done
echo "麒麟适配里程碑文档" > "$TREE/验收标准.md"

dbus-run-session bash -c '
  set -e
  '"$D"' --root '"$TREE"' --db '"$BASE"'/index.bin --socket '"$BASE"'/d.sock --dbus '"$BASE"'/dbus.log > '"$BASE"'/daemon.log 2>&1 &
  DPID=$!
  for i in $(seq 1 60); do
    [ -S '"$BASE"'/d.sock ] && '"$CLI"' status --socket '"$BASE"'/d.sock >/dev/null 2>&1 && break
    sleep 0.1
  done
  echo "== Ping =="
  dbus-send --print-reply --dest=org.kysearch.Daemon /org/kysearch/Daemon org.kysearch.Daemon.Ping | head -3
  echo "== Search 手机 =="
  dbus-send --print-reply --dest=org.kysearch.Daemon /org/kysearch/Daemon org.kysearch.Daemon.Search string:"" string:"手机" > '"$BASE"'/reply.txt
  echo "== Search ext:md =="
  dbus-send --print-reply --dest=org.kysearch.Daemon /org/kysearch/Daemon org.kysearch.Daemon.Search string:"" string:"ext:md" | grep -c "验收标准" || true
  '"$CLI"' stop --socket '"$BASE"'/d.sock >/dev/null 2>&1 || kill $DPID 2>/dev/null || true
' 2>&1 | tail -20

N=$(grep -c '手机' "$BASE/reply.txt" || true)
TOTAL=$(awk '/uint32/{print $2; exit}' "$BASE/reply.txt")
[ "$TOTAL" = "31" ] || fail "总数应为 31（30 文件+目录），实际 $TOTAL"
[ "$N" -ge 30 ] || fail "路径条目应 >=30，实际 $N"
pass "D-Bus Search 返回 total=$TOTAL, 条目 $N"
pass "DBUS TESTS PASSED"
rm -rf "$BASE"
