#!/usr/bin/env bash
# 端到端集成测试：生成 3 万文件合成树 → 建索引 → 各搜索模式断言 → 与 find 交叉验证
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="${BUILD:-$HERE/../build}"
CLI="$BUILD/kysearch-cli"
TREE="${TREE:-/tmp/kysearch-tree}"
DB="${DB:-/tmp/kysearch-test.bin}"

fail() { echo "✗ 断言失败: $1"; exit 1; }
pass() { echo "✓ $1"; }

echo "== 1. 生成合成测试树（3 万文件，含中文/空格/大小写/深层目录）"
rm -rf "$TREE" "$DB"
python3 "$HERE/gen_tree.py" "$TREE" > /tmp/kysearch-expect.env
source /tmp/kysearch-expect.env

echo "== 2. 全量扫描建索引（2 线程）"
"$CLI" scan "$TREE" --db "$DB" --threads 2

count() { "$CLI" search --db "$DB" "$@" | awk '!/^#/{n++} END{print n+0}'; }
assert_eq() {
  local name=$1 expect=$2; shift 2
  local got; got=$(count "$@")
  [ "$got" -eq "$expect" ] || fail "$name: 期望 $expect 实际 $got ($*)"
  pass "$name = $got"
}

echo "== 3. 搜索断言"
assert_eq "中文子串 手机"        "$EXPECT_PHONE"             手机
assert_eq "多词 AND 报告 docx"   "$EXPECT_BAOGAO_DOCX"       报告 docx
assert_eq "区分大小写 IMG_2025"  "$EXPECT_IMG_CASE"          --case IMG_2025
assert_eq "区分大小写 img_2025"  "$EXPECT_IMG_CASE_LOWER"    --case img_2025
assert_eq "全字匹配 report"      "$EXPECT_WORD_REPORT"       --word report
assert_eq "普通子串 report"      "$EXPECT_REPORT_ANY"        report
assert_eq "路径模式 assets/icons" "$EXPECT_PATH_ASSETS_ICONS" --path assets/icons
assert_eq "文件名模式 assets"    "$EXPECT_NAME_ASSETS"       assets
assert_eq "icon_"                "$EXPECT_ICON"              icon_
assert_eq "中文 深层"            "$EXPECT_DEEP"              深层
assert_eq "拼音全拼 shouji"      "$EXPECT_PY_SHOUJI"        shouji
assert_eq "拼音首字母 sj"        "$EXPECT_PY_SJ"            sj
assert_eq "拼音全拼 baogao"      "$EXPECT_PY_BAOGAO"        baogao
assert_eq "拼音首字母 bg"        "$EXPECT_PY_BG"            bg
assert_eq "拼音首字母 scsj"      "$EXPECT_PY_SCSJ"          scsj

echo "== 4. 与 find 交叉验证（ground truth）"
gt1=$(find "$TREE" -name '*手机*' | wc -l)
[ "$gt1" -eq "$EXPECT_PHONE" ] || fail "find 手机: $gt1 != $EXPECT_PHONE"
pass "find 交叉验证 手机 = $gt1"
gt2=$(find "$TREE" -name '*深层*' | wc -l)
[ "$gt2" -eq "$EXPECT_DEEP" ] || fail "find 深层: $gt2 != $EXPECT_DEEP"
pass "find 交叉验证 深层 = $gt2"
gt_all=$(find "$TREE" -mindepth 1 | wc -l)
[ "$gt_all" -eq "$EXPECT_TOTAL" ] || fail "find 总数: $gt_all != $EXPECT_TOTAL"
pass "find 交叉验证 总条目 = $gt_all"

echo "== 5. 快照加载与信息"
"$CLI" info --db "$DB"

echo ""
echo "ALL INTEGRATION TESTS PASSED"
