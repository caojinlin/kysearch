#!/usr/bin/env bash
# 构建正式 deb 包（x86_64 / aarch64 通用；飞腾鲲鹏请先用 cross-aarch64.sh 产物）
# 用法: packaging/make-deb.sh [--arch amd64|arm64] [--bin-dir PATH]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="amd64"
BIN_DIR="$ROOT/build"
while [ $# -gt 0 ]; do
  case "$1" in
    --arch) ARCH="$2"; shift 2 ;;
    --bin-dir) BIN_DIR="$2"; shift 2 ;;
    *) echo "未知参数 $1"; exit 2 ;;
  esac
done

VER="0.2.0"
STAGE="$(mktemp -d)/kysearch"
trap 'rm -rf "$(dirname "$STAGE")"' EXIT

install -d "$STAGE/DEBIAN" \
  "$STAGE/usr/bin" \
  "$STAGE/usr/share/applications" \
  "$STAGE/usr/share/icons/hicolor/scalable/apps" \
  "$STAGE/usr/lib/systemd/user"

for bin in kysearch-cli kysearch-gui kysearchd; do
  [ -f "$BIN_DIR/$bin" ] && install -m 755 "$BIN_DIR/$bin" "$STAGE/usr/bin/"
done
install -m 644 "$ROOT/packaging/kysearch.desktop" "$STAGE/usr/share/applications/"
install -m 644 "$ROOT/packaging/kysearch.svg" \
  "$STAGE/usr/share/icons/hicolor/scalable/apps/"
install -m 644 "$ROOT/packaging/systemd/kysearch.service" \
  "$STAGE/usr/lib/systemd/user/"

case "$ARCH" in
  amd64) DEPS="libqt5widgets5 (>= 5.9), libqt5network5 (>= 5.9), libre2-5 | libre2-9 | libre2-10, libsqlite3-0" ;;
  arm64) DEPS="" ;;  # 静态交叉产物无动态依赖
esac

if [ -f "$ROOT/packaging/deb/DEBIAN/postinst" ]; then
  install -m 755 "$ROOT/packaging/deb/DEBIAN/postinst" "$STAGE/DEBIAN/postinst"
fi

cat > "$STAGE/DEBIAN/control" <<EOF
Package: kysearch
Version: $VER
Section: utils
Priority: optional
Architecture: $ARCH
Depends: $DEPS
Maintainer: KySearch Team <dev@example.invalid>
Description: 麒麟快搜 —— 秒级全盘文件名搜索工具（Everything 风格）
 内存索引 + 并行扫描 + 实时监控（fanotify/inotify/对账三级降级）；
 支持即时子串/多词 AND/大小写/全字/全路径/正则(RE2)/过滤器语法
 (ext: size: dm: type: path:)/中文拼音全拼首字母/全文内容搜索(trigram)；
 含守护进程 kysearchd（systemd 用户服务）与 Qt5 图形界面。
Homepage: https://example.invalid/kysearch
EOF

OUT="$ROOT/kysearch_${VER}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "$STAGE" "$OUT"
echo "打包完成: $OUT ($(du -h "$OUT" | cut -f1))"
ls -la "$OUT"
