# 麒麟（银河麒麟 V10 / UKUI）适配与上架指南

## 1. 依赖矩阵

| 依赖 | 版本要求 | V10 SP1 自带 | 说明 |
|---|---|---|---|
| g++ / clang | C++17 | ✅ | 核心引擎纯标准库 |
| Qt5 Widgets/Network | ≥ 5.9 | ✅（UKUI 同源 Qt5） | 仅 GUI 需要 |
| libre2 | ≥ 9 | ⚠️ 需安装 | 正则搜索（缺失则自动禁用，不挡编译） |
| libsqlite3（FTS5） | ≥ 3.34 | ⚠️ 需安装 | 全文搜索（缺失则自动禁用） |
| libx11 | 任意 | ✅ | 全局热键（缺失自动禁用） |

安装依赖（麒麟/Ubuntu 同源 apt）：

```bash
sudo apt install -y build-essential cmake qtbase5-dev \
                    libre2-dev libsqlite3-dev libx11-dev
```

## 2. 构建与安装

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo dpkg -i kysearch_0.2.0_amd64.deb        # 或 make-deb.sh 自行打包
systemctl --user enable --now kysearch        # 守护进程（实时索引）
kysearch-gui --remote                          # 图形界面连守护进程
```

无守护进程也可独立运行（快照模式，启动时自动扫描 $HOME）：
`kysearch-gui`

## 3. 监控级别与权限（三级降级链）

守护进程启动时自动探测，逐级降级，`kysearch-cli status` 可查当前级别：

| 级别 | 机制 | 权限要求 | 麒麟场景 |
|---|---|---|---|
| L1 fanotify | 文件系统级标记 + 句柄解析 | root（CAP_DAC_READ_SEARCH） | 以系统服务部署时可用 |
| L2 inotify | 目录级 watch | 普通用户 | 用户服务的默认级别 |
| L3 定时对账 | 低频全量重扫比对 | 普通用户 | 永远兜底运行（修复事件漂移） |

- 用户服务（推荐）：L2+L3，零配置。
- 想要 L1：将 unit 安装为系统服务（`/etc/systemd/system`，`User=root`），
  或直接 `sudo kysearchd --level fanotify ...`。仓库 smoke 测试：
  `sudo tests/fanotify-root.sh`。
- inotify watch 上限不足时守护进程会记录告警并依赖对账补齐；建议：
  `echo fs.inotify.max_user_watches=1048576 | sudo tee -a /etc/sysctl.conf`

## 4. 多架构（飞腾/鲲鹏/龙芯）

| 架构 | 方案 | 状态 |
|---|---|---|
| x86_64（兆芯/海光/Intel） | 本机构建 | ✅ 已验证 |
| aarch64（飞腾/鲲鹏） | `packaging/cross-aarch64.sh` 交叉编译 + QEMU 实测 | ✅ 已验证 |
| loongarch64（龙芯） | 需麒麟软件源或 loongnix 交叉工具链（`loongarch64-linux-gnu-g++`），流程同 aarch64 脚本；GUI 需目标架构 Qt5 | 📄 流程就绪 |

核心引擎与 CLI/守护进程不依赖 Qt，可用 `-DKYSEARCH_MINIMAL=ON -static`
静态编译后直接部署；GUI 在目标机上原生构建即可（同一套 CMake）。

## 5. UKUI 桌面集成

- desktop 入口已内置（deb 安装后出现在开始菜单「系统工具」）；
- 主题：菜单「视图→主题」与托盘菜单均支持跟随系统/浅色/深色；
- 全局热键 Ctrl+Alt+K 唤起/隐藏；有系统托盘时关窗缩到托盘；
- 左侧类型过滤栏（全部/文件夹/文档/表格/演示/PDF/图片/视频/音频/
  压缩文件，含 WPS 扩展名 wps/et/dps），与搜索词叠加；下方为当前
  结果的分布聚合（点击追加过滤器）；
- 设置页支持多索引根目录列表管理；守护进程对各根独立实时监控；
  以 systemd 用户服务运行时保存设置自动重启生效，GUI 断线自动重连；
- 托盘：单击唤起；右键菜单含索引状态行、最近搜索、快捷过滤、
  重建索引、守护进程状态、主题、开机自启（界面 desktop 文件 +
  守护进程 systemd 用户服务）、设置；Tooltip 实时显示索引/连接状态；
- 右键结果「打开所在文件夹」走 freedesktop FileManager1.ShowItems
  （麒麟 peony 支持），在文件管理器中定位并选中该文件；
- Wayland 会话下全局热键不可用（X11 会话正常），界面功能不受影响。

## 6. 麒麟软件商店上架材料清单

1. deb 包（多架构）+ 签名；
2. 软件截图（620×400 以上 ×4）与应用描述；
3. 软件著作权信息、开发商资质；
4. 适配测试报告：麒麟版本（V10 SP1/SP2/SP3）× CPU（x86_64/飞腾/鲲鹏/龙芯）
   矩阵的安装-启动-核心功能记录（可直接引用本仓库 tests/ 输出）；
5. 联系人与售后支持方式。
审核要点：不要求 root（默认用户态运行）、无危险权限、明示索引范围与
内容搜索开关（--content-roots 默认关闭）。

## 7. 安全说明

- 守护进程以普通用户运行，只索引该用户可读文件；打开文件时二次校验；
- 索引与快照存放于 `~/.cache/kysearch/`，卸载即删；
- 全文内容索引默认关闭，启用时仅索引白名单纯文本扩展名且单文件 ≤2MB；
- socket 文件权限 0666（查询接口），位于用户目录，生产可收紧目录权限。

## 8. 故障排查

| 现象 | 处理 |
|---|---|
| 搜不到刚建的文件 | `kysearch-cli status` 看级别；L3 模式等待对账周期或 `RESCAN` |
| fanotify 不生效 | 需要 root；普通用户自动降级 inotify 属预期 |
| 中文输入无结果 | 拼音索引在扫描时生成；旧快照需「视图→重建索引」 |
| 内存偏高 | 百万文件约 150–300MB；低配机建议缩小 --root 范围 |
