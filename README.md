# 麒麟快搜 · KySearch

面向银河麒麟（UKUI/Qt）的 Everything 风格文件秒搜工具。Linux 没有 NTFS 的
MFT/USN Journal，本项目用 **并行扫描 + 全内存索引 + 实时监控三级降级链**
逼近 Everything 的体验，并针对中文场景增加 **拼音搜索** 与 **全文内容搜索**。

## 下载安装

**[⬇ 最新 Release 下载](https://github.com/caojinlin/kysearch/releases)** ·
[直接下载 kysearch_0.2.0_amd64.deb](https://github.com/caojinlin/kysearch/releases/download/v0.2.0/kysearch_0.2.0_amd64.deb)

```bash
# 麒麟 V10（x86_64）安装：
wget https://github.com/caojinlin/kysearch/releases/download/v0.2.0/kysearch_0.2.0_amd64.deb
sudo dpkg -i kysearch_0.2.0_amd64.deb && sudo apt-get install -f -y

# 启用实时索引守护进程（推荐）：
systemctl --user enable --now kysearch
loginctl enable-linger $USER     # 注销后仍保持索引（可选）

kysearch-gui --remote            # 打开界面（连接守护进程，实时索引）
```

或从源码构建（依赖与多架构说明见 [docs/ADAPTATION.md](docs/ADAPTATION.md)）。

## 功能全景（M1–M4 + 优化轮全部交付）

| 能力 | 说明 |
|---|---|
| 即时搜索 | 输入即出（120ms 防抖 + worker 线程异步，百万项不卡 UI） |
| 搜索方式 | 子串 / 多词 AND / 大小写 / 全字 / 全路径 / **正则（RE2 零拷贝）** |
| **布尔运算** | `a \| b`（OR）、`!a`（排除）、`( )` 分组，与过滤器自由组合 |
| **重复文件** | `kysearch-cli dup`：size→首 64KB 哈希→全文件哈希三级收敛 |
| **CSV 导出** | CLI `--csv PATH` / GUI 视图菜单，全量命中导出 |
| **预览面板** | 选中文本/代码/图片即时预览（右侧可停靠面板） |
| **设置与自启** | GUI 设置页（根目录/排除/全文范围）生成 kysearchd.conf；开机自启开关 |
| **D-Bus 生态** | `org.kysearch.Daemon`（Search/Ping），供 UKUI 搜索面板等集成 |
| **并发查询** | shared_mutex 读共享写独占 + 独立缓存锁（v4） |
| 过滤器语法 | `ext:` `size:>10mb` `dm:today\|7d\|A..B` `type:` `path:` |
| **中文拼音** | 全拼 + 首字母 + 多音组合 + **模糊音（z/zh c/ch s/sh n/l f/h）** |
| **全文内容** | `content:关键词`，FTS5 trigram，事件驱动 + mtime 跳过 |
| 实时索引 | fanotify→inotify→对账三级降级；改名/增删 ≈1s 可搜；**对账 swap 回放日志堵住扫描窗口竞态** |
| 守护进程 | kysearchd + Unix socket IPC；监控批次后节流重整（rank/倒排） |
| GUI | 虚拟表格 + **列排序** + **回收站删除(freedesktop)** + **类型快速过滤侧栏** + **筛选分布** + 托盘 + Ctrl+Alt+K 热键 + 搜索历史 + 亮暗主题 + 本地/远程双模式 + **打开所在文件夹并选中（FileManager1）** + **断线自动重连** |
| 类型过滤 | 左侧栏单选分类：全部/文件夹/文档/表格/演示/PDF/图片/视频/音频/压缩文件（含 WPS 扩展名），与搜索词自由叠加；下方动态分布（扩展名/目录 Top）点击追加 |
| 托盘 | 右键：**索引状态行** / 最近搜索 / 快捷过滤（今天/7天/大文件/文档/图片/视频/文件夹/下载） / 重建索引 / **守护进程状态** / 主题 / **开机自启（界面+守护进程）** / 设置；**单击唤起**；Tooltip 实时索引状态 |
| 多根索引 | 设置页列表式管理（添加/浏览/移除）；守护进程 `--root` 可重复、`roots=a,b,c` 配置；各根独立实时监控；保存后 systemd 服务自动重启生效、界面自动重连 |
| 快照 v4 | 原子落盘(tmp+fsync+rename)；**倒排与 rank 随快照持久化，加载免重建** |
| 性能结构 | 扁平倒排 + 增量溢出区 + rank 整数排序 + 打字增量缓存 + 路径目录子树化 + **v4 去 lpath 冗余（内存 -23%）** |
| 多架构 | x86_64 原生；aarch64 交叉+QEMU 实测；loongarch64 流程就绪 |

## 架构

```
┌──────────────── kysearch-gui（Qt Widgets） ─────────────────┐
│ 搜索框 │ 路径/Aa/全字/.* 开关 │ 虚拟表格 │ 历史/主题/托盘/热键 │
│   本地模式：内嵌引擎+快照      远程模式：QLocalSocket 分页拉取 │
└──────────────▲───────────────────────────▲──────────────────┘
               │ Unix socket 行协议（PING/STATUS/SEARCH/RESCAN/STOP）
┌──────────────┴──────── kysearchd ─────────┴─────────────────┐
│ 查询引擎（2-gram 倒排 + 过滤器 + RE2 + 拼音键）               │
│ 全文索引（SQLite FTS5 trigram，事件驱动）                    │
│ 监控三级降级链：fanotify(L1,root) → inotify(L2) → 对账(L3)   │
│ 扫描器：多线程 BFS │ 周期快照 │ systemd 用户服务              │
└──────────────────────────────────────────────────────────────┘
```

## 快速开始

```bash
# 构建（依赖见 docs/ADAPTATION.md）
sudo apt install -y build-essential cmake pkg-config qtbase5-dev libre2-dev libsqlite3-dev libx11-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j2

# 一分钟体验
./build/kysearch-cli scan ~/some/dir --progress        # 建索引
./build/kysearch-cli search 手机                        # 中文子串
./build/kysearch-cli search shouji                      # 拼音全拼
./build/kysearch-cli search "ext:docx dm:today 报告"     # 过滤器 + AND
./build/kysearch-cli search --regex '^IMG_\d{4}'         # 正则
./build/kysearch-cli search 手机 --csv out.csv             # CSV 导出
./build/kysearch-cli dup --db ~/.cache/kysearch/index.bin  # 重复文件查找
kysearchd --config ~/.config/kysearch/kysearchd.conf --dbus  # 配置文件 + D-Bus
./build/kysearchd --root ~/some/dir &                   # 守护进程（实时）
./build/kysearch-cli search --socket ~/.cache/kysearch/daemon.sock 新文件
./build/kysearch-gui --remote                           # GUI 实时索引模式
```

## 测试矩阵（全部通过）

```bash
cd build && ctest                # 单元测试：扫描/搜索/过滤/正则/拼音/增量/快照
tests/integration.sh             # 3 万文件集成 + find 交叉验证 + 拼音断言
tests/daemon.sh                  # 守护进程端到端：实时增删改名/目录树/全文/对账/退出
tests/rescan-race.sh             # 对账 swap × 实时变更竞态压测（1s 对账 × 8 轮风暴零丢失）
sudo tests/fanotify-root.sh      # L1 fanotify root 冒烟（文件系统级监控）
packaging/cross-aarch64.sh       # aarch64 交叉编译 + QEMU 实测
```

实测数据（本 2 核开发机 / 30509 项合成树 / 10 万项 CJK 树）：

| 指标 | 数值 |
|---|---|
| 全量扫描（30k 混合 / 100k 全中文名） | 95 ms / 688 ms（含拼音+模糊音键生成） |
| 命中 5000 项查询 | 0.8–1.2 ms（x100 轮） |
| 无命中查询 | <0.01 ms（倒排快速拒绝） |
| 路径模式（10 万项全中文名树） | 14.8 ms（命中 10 万；稀疏命中时由目录子树直接定位） |
| 布尔 OR（命中 10001） | 1 ms（GUI 实测） |
| 文件变更 → 可搜 | ≈1s（inotify + 200ms 去抖） |
| 快照加载（10 万项，倒排随盘） | 50 ms |

## 目录结构

```
src/core/     index(索引/查询/快照) scanner(并行扫描) monitor(三级监控)
              pinyin(拼音键) textindex(FTS5 全文)
src/daemon/   kysearchd：IPC 服务 + 监控 + 内容索引 + 周期落盘
src/cli/      命令行：scan/search/info/status/stop，支持 --socket 远程
src/gui/      Qt 界面：mainwindow/resultmodel/hotkey(X11 全局热键)
tests/        test_core 单元 / gen_tree 合成树 / integration|daemon|fanotify-root 脚本
packaging/    make-deb.sh 打包 / cross-aarch64.sh 交叉 / desktop / svg / systemd
tools/        gen_pinyin.py 拼音表生成（pypinyin）
docs/         ADAPTATION.md 麒麟适配与商店上架指南
```

## 设计要点

- **索引**：arena×4（小写名/小写路径/原始路径/拼音键）+ 41B Entry +
  名与拼音共用 2-gram 倒排；查询取最小 posting 驱动 + 原文验证；读写共锁。
- **增量**：alive 位图 + lpath 哈希定位；upsert 即时插 gram（升序不变式）；
  目录删除走前缀清尾；对账重扫原子 swap（顺带压缩墓碑）。
- **拼音**：默认音全拼 + 首字母 + 多音组合（上限 8/16 防爆炸），
  变体以 \x1f 拼接——控制字符不可输入，无跨界误匹配；首字母为子串语义
  （`sj` 也会命中 `scsj`），属预期行为。
- **正则**：RE2 匹配原始大小写文本（`(?i)` 可忽略大小写），线性时间。
- **全文**：trigram 分词带来子串语义（比词法分词更适合"找关键词"）；
  白名单扩展名 + 2MB 上限 + 默认关闭。
- **热键**：独立 Xlib 连接线程 XGrabKey（含 NumLock 变体），Wayland 自动降级。

## 许可与声明

拼音数据由 pypinyin 生成（MIT）。其余代码为本项目原创，供麒麟生态使用。
