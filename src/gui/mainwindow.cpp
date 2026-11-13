#include "gui/mainwindow.h"

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCompleter>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPalette>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QProcess>
#include <functional>
#include <algorithm>
#include <cstdio>
#include <QStatusBar>
#include <QStringListModel>
#include <QSystemTrayIcon>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QLocalSocket>
#ifdef KYSEARCH_HAVE_QTDBUS
#include <QDBusInterface>
#include <QDBusReply>
#endif
#include <QDockWidget>
#include <QListWidget>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QFileDialog>
#include <QFormLayout>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTextEdit>

#include "core/fsutil.h"
#include "core/pinyin.h"
#include "core/scanner.h"
#ifdef KYSEARCH_HAVE_SQLITE
#include "core/textindex.h"
#endif
#include "gui/hotkey.h"
#include "gui/resultmodel.h"

namespace {
QString human_size_q(qint64 b) {
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  int u = 0;
  double v = (double)b;
  while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
  return u == 0 ? QString("%1 B").arg(b) : QString("%1 %2").arg(v, 0, 'f', 1).arg(units[u]);
}

// ---- 开机自启（托盘与设置页共用同一 desktop 文件）----
QString gui_autostart_file() {
  return QDir::homePath() + QStringLiteral("/.config/autostart/kysearch-gui.desktop");
}
void set_gui_autostart(bool on) {
  if (on) {
    QDir().mkpath(QFileInfo(gui_autostart_file()).absolutePath());
    QFile af(gui_autostart_file());
    if (af.open(QIODevice::WriteOnly | QIODevice::Text))
      af.write("[Desktop Entry]\nType=Application\nName=麒麟快搜\n"
               "Exec=kysearch-gui\nTerminal=false\nX-GNOME-Autostart-enabled=true\n");
  } else {
    QFile::remove(gui_autostart_file());
  }
}
// systemctl --user（守护进程以用户服务方式安装时可用）
bool daemon_autostart_enabled() {
  QProcess p;
  p.start(QStringLiteral("systemctl"),
          {QStringLiteral("--user"), QStringLiteral("is-enabled"),
           QStringLiteral("kysearch")});
  if (!p.waitForFinished(3000)) { p.kill(); return false; }
  return p.exitCode() == 0;
}
int set_daemon_autostart(bool on) {
  QProcess p;
  p.start(QStringLiteral("systemctl"),
          {QStringLiteral("--user"),
           on ? QStringLiteral("enable") : QStringLiteral("disable"),
           QStringLiteral("kysearch")});
  if (!p.waitForFinished(5000)) { p.kill(); return -1; }
  return p.exitCode();
}
}  // namespace

MainWindow::MainWindow(const Options& opt, QWidget* parent)
    : QMainWindow(parent), opt_(opt) {
  defaultPalette_ = QApplication::palette();
  buildUi();
  applyTheme(opt_.theme);
  setupTray();
  setupHotkey();
  if (opt_.remote) {
    sock_ = new QLocalSocket(this);
    connect(sock_, &QLocalSocket::connected, this,
            &MainWindow::onRemoteConnected);
    connect(sock_, &QLocalSocket::readyRead, this,
            &MainWindow::onRemoteReadyRead);
    connect(sock_, &QLocalSocket::disconnected, this,
            &MainWindow::onRemoteDisconnected);
    // 断线自动重连（守护进程重启/升级期间无感恢复，如设置保存后的自动重启）
    reconnectTimer_ = new QTimer(this);
    reconnectTimer_->setInterval(3000);
    connect(reconnectTimer_, &QTimer::timeout, this, [this] {
      if (shuttingDown_) return;
      if (sock_->state() == QLocalSocket::ConnectedState) {
        reconnectTimer_->stop();
        return;
      }
      leftStatus_->setText(tr("⟳ 重连守护进程…"));
      sock_->abort();
      sock_->connectToServer(opt_.socket);
    });
    leftStatus_->setText(tr("⟳ 连接守护进程 %1 …").arg(opt_.socket));
    sock_->connectToServer(opt_.socket);
    reconnectTimer_->start();  // 首连失败也由该定时器兜底重试
  } else {
    tryLoadDbOrScan();
  }
}

MainWindow::~MainWindow() {
  shuttingDown_ = true;
  ++searchGen_;  // 作废在途异步搜索
  if (searchThread_.joinable()) searchThread_.join();
  // 先静默 socket：其析构会同步发 disconnected，此时子控件已析构
  if (sock_) {
    sock_->blockSignals(true);
    sock_->close();
  }
  if (hotkey_) hotkey_->stop();
  if (scanThread_.joinable()) scanThread_.join();
}

void MainWindow::buildUi() {
  setWindowTitle(tr("麒麟快搜 · KySearch"));
  resize(1000, 640);

  auto* central = new QWidget(this);
  auto* rootLayout = new QVBoxLayout(central);
  rootLayout->setContentsMargins(8, 8, 8, 8);
  rootLayout->setSpacing(6);

  // ---- 顶栏：搜索框 + 模式开关 ----
  auto* top = new QHBoxLayout();
  searchEdit_ = new QLineEdit(central);
  searchEdit_->setPlaceholderText(
      tr("输入即搜索：空格=AND；ext:docx size:>10mb dm:today type:dir "
         "content:关键词；拼音 shouji/sj"));
  searchEdit_->setClearButtonEnabled(true);
  QFont f = searchEdit_->font();
  f.setPointSize(11);
  searchEdit_->setFont(f);

  pathBtn_ = new QToolButton(central);
  pathBtn_->setText(tr("路径"));
  pathBtn_->setToolTip(tr("按全路径搜索（默认只搜文件名）"));
  caseBtn_ = new QToolButton(central);
  caseBtn_->setText("Aa");
  caseBtn_->setToolTip(tr("区分大小写"));
  wordBtn_ = new QToolButton(central);
  wordBtn_->setText(tr("全字"));
  wordBtn_->setToolTip(tr("全字匹配（边界不是字母/数字/下划线）"));
  regexBtn_ = new QToolButton(central);
  regexBtn_->setText(".*");
  regexBtn_->setToolTip(tr("正则模式（RE2，防回溯卡死；原始大小写，可用 (?i)）"));
  for (auto* b : {pathBtn_, caseBtn_, wordBtn_, regexBtn_}) {
    b->setCheckable(true);
    b->setAutoRaise(true);
    connect(b, &QToolButton::toggled, this, [this](bool) {
      debounce_->start();  // 开关切换立即重搜（原需重新输入才生效）
    });
  }

  top->addWidget(searchEdit_, 1);
  top->addWidget(pathBtn_);
  top->addWidget(caseBtn_);
  top->addWidget(wordBtn_);
  top->addWidget(regexBtn_);

  // ---- 结果表 ----
  model_ = new ResultModel(this);
  view_ = new QTableView(central);
  view_->setModel(model_);
  view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  view_->setSelectionBehavior(QAbstractItemView::SelectRows);
  view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  view_->setAlternatingRowColors(true);
  view_->verticalHeader()->setDefaultSectionSize(26);
  view_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  view_->setColumnWidth(0, 280);
  view_->setColumnWidth(2, 90);
  view_->setColumnWidth(3, 140);
  view_->setColumnWidth(4, 64);
  view_->setContextMenuPolicy(Qt::CustomContextMenu);

  rootLayout->addLayout(top);
  rootLayout->addWidget(view_, 1);
  setCentralWidget(central);

  // ---- 菜单：主题 ----
  auto* viewMenu = menuBar()->addMenu(tr("视图"));
  auto* themeMenu = viewMenu->addMenu(tr("主题"));
  auto* themeGroup = new QActionGroup(this);
  const QStringList themeNames = {tr("跟随系统"), tr("浅色"), tr("深色")};
  for (int i = 0; i < themeNames.size(); ++i) {
    auto* act = themeMenu->addAction(themeNames[i]);
    act->setCheckable(true);
    act->setChecked(i == 0);
    act->setActionGroup(themeGroup);
    const int mode = i;
    connect(act, &QAction::triggered, this, [this, mode] {
      applyTheme(mode);
      QSettings s;
      s.setValue("theme", mode);
    });
  }
  viewMenu->addAction(tr("重建索引"), this, [this] { rebuildIndex(); });
  viewMenu->addAction(tr("类型过滤与分布侧栏"), this, [this] {
    facetsDock_->setVisible(!facetsDock_->isVisible());
  });
  viewMenu->addAction(tr("预览面板"), this, [this] {
    previewDock_->setVisible(!previewDock_->isVisible());
  });
  viewMenu->addAction(tr("导出结果为 CSV…"), this, [this] { exportCsv(); });
  viewMenu->addAction(tr("设置…"), this, [this] { openSettings(); });

  // ---- 侧栏：类型快速过滤（固定分类）+ 筛选分布（动态聚合）----
  facetsDock_ = new QDockWidget(tr("类型过滤与分布"), this);
  auto* facetsHost = new QWidget(facetsDock_);
  auto* facetsLay = new QVBoxLayout(facetsHost);
  facetsLay->setContentsMargins(4, 6, 4, 4);
  facetsLay->setSpacing(2);
  auto* quickLabel = new QLabel(tr("按类型快速过滤"), facetsHost);
  quickLabel->setStyleSheet("font-weight: bold; color: gray;");
  facetsLay->addWidget(quickLabel);

  quickList_ = new QListWidget(facetsHost);
  quickList_->setWordWrap(true);
  struct Cat { const char* label; const char* icon; const char* syntax; };
  const Cat cats[] = {
      {QT_TRANSLATE_NOOP("MainWindow", "全部"), "system-search", ""},
      {QT_TRANSLATE_NOOP("MainWindow", "文件夹"), "folder", "type:dir"},
      {QT_TRANSLATE_NOOP("MainWindow", "文档"),
       "x-office-document", "ext:doc,docx,wps,rtf,odt"},
      {QT_TRANSLATE_NOOP("MainWindow", "表格"),
       "x-office-spreadsheet", "ext:xls,xlsx,et,csv,ods"},
      {QT_TRANSLATE_NOOP("MainWindow", "演示"),
       "x-office-presentation", "ext:ppt,pptx,dps,odp"},
      {"PDF", "application-pdf", "ext:pdf"},
      {QT_TRANSLATE_NOOP("MainWindow", "图片"), "image-x-generic",
       "ext:jpg,jpeg,png,gif,bmp,webp,svg,tiff,heic,ico"},
      {QT_TRANSLATE_NOOP("MainWindow", "视频"), "video-x-generic",
       "ext:mp4,mkv,avi,mov,wmv,flv,ts,webm,m4v"},
      {QT_TRANSLATE_NOOP("MainWindow", "音频"), "audio-x-generic",
       "ext:mp3,wav,flac,ape,aac,ogg,wma,m4a"},
      {QT_TRANSLATE_NOOP("MainWindow", "压缩文件"), "application-x-archive",
       "ext:zip,rar,7z,tar,gz,bz2,xz,iso"},
  };
  for (const auto& c : cats) {
    auto* it = new QListWidgetItem(QIcon::fromTheme(c.icon),
                                   tr(c.label), quickList_);
    it->setData(Qt::UserRole, QString::fromLatin1(c.syntax));
    it->setToolTip(
        c.syntax[0] ? tr("%1（%2）").arg(tr(c.label)).arg(c.syntax) : QString());
  }
  quickList_->setCurrentRow(0);
  // 高度按行数固定，保证 10 个分类全部可见（不出现滚动裁剪）
  quickList_->setFixedHeight(
      quickList_->sizeHintForRow(0) * (int)(sizeof(cats) / sizeof(cats[0])) + 6);
  facetsLay->addWidget(quickList_);
  connect(quickList_, &QListWidget::currentRowChanged, this, [this](int) {
    const QString syn = quickList_->currentItem()
                            ? quickList_->currentItem()->data(Qt::UserRole).toString()
                            : QString();
    if (syn == typeFilter_) return;
    typeFilter_ = syn;
    debounce_->start();  // 切换分类立即重搜（与搜索词叠加）
  });
  // 自动化测试钩子：KYSEARCH_GUI_TYPE=<行号> 程序化选择分类（走同一信号路径）
  if (const char* tn = std::getenv("KYSEARCH_GUI_TYPE")) {
    const int row = std::atoi(tn);
    QTimer::singleShot(500, this, [this, row] { quickList_->setCurrentRow(row); });
  }

  auto* facetLabel = new QLabel(tr("筛选分布（当前结果）"), facetsHost);
  facetLabel->setStyleSheet("font-weight: bold; color: gray;");
  facetsLay->addWidget(facetLabel);
  facetList_ = new QListWidget(facetsHost);
  facetList_->setWordWrap(true);
  facetsLay->addWidget(facetList_, 1);
  facetsDock_->setWidget(facetsHost);
  facetsDock_->setFeatures(QDockWidget::DockWidgetMovable |
                           QDockWidget::DockWidgetClosable);
  addDockWidget(Qt::LeftDockWidgetArea, facetsDock_);
  facetsDock_->show();  // 默认显示（类型快速过滤入口）
  connect(facetList_, &QListWidget::itemClicked, this,
          [this](QListWidgetItem* it) { applyFacet(it->data(Qt::UserRole).toString()); });

  // ---- 预览面板 ----
  previewDock_ = new QDockWidget(tr("预览"), this);
  previewStack_ = new QStackedWidget(previewDock_);
  previewText_ = new QPlainTextEdit(previewStack_);
  previewText_->setReadOnly(true);
  previewImage_ = new QLabel(previewStack_);
  previewImage_->setAlignment(Qt::AlignCenter);
  previewInfo_ = new QLabel(previewStack_);
  previewInfo_->setAlignment(Qt::AlignCenter);
  previewInfo_->setWordWrap(true);
  previewStack_->addWidget(previewInfo_);
  previewStack_->addWidget(previewText_);
  previewStack_->addWidget(previewImage_);
  previewDock_->setWidget(previewStack_);
  previewStack_->setMinimumWidth(300);
  previewText_->setMinimumWidth(300);
  previewImage_->setMinimumWidth(300);
  previewInfo_->setMinimumWidth(300);
  addDockWidget(Qt::RightDockWidgetArea, previewDock_);
  connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged,
          this, [this] { updatePreview(); });
  connect(previewDock_, &QDockWidget::visibilityChanged, this,
          [this](bool) { updatePreview(); });

  // ---- 状态栏 ----
  leftStatus_ = new QLabel(this);
  rightStatus_ = new QLabel(this);
  statusBar()->addWidget(leftStatus_, 1);
  statusBar()->addPermanentWidget(rightStatus_);
  leftStatus_->setText(tr("⟳ 正在初始化…"));

  // ---- 搜索历史 ----
  historyModel_ = new QStringListModel(this);
  QSettings st;
  historyModel_->setStringList(st.value("history").toStringList());
  completer_ = new QCompleter(historyModel_, this);
  completer_->setCaseSensitivity(Qt::CaseInsensitive);
  completer_->setCompletionMode(QCompleter::InlineCompletion);
  searchEdit_->setCompleter(completer_);

  // ---- 行为 ----
  debounce_ = new QTimer(this);
  debounce_->setInterval(120);
  debounce_->setSingleShot(true);
  connect(searchEdit_, &QLineEdit::textChanged, debounce_,
          qOverload<>(&QTimer::start));
  connect(debounce_, &QTimer::timeout, this, &MainWindow::onDebouncedSearch);
  connect(searchEdit_, &QLineEdit::returnPressed, this,
          &MainWindow::onReturnPressed);
  for (auto* b : {pathBtn_, caseBtn_, wordBtn_, regexBtn_})
    connect(b, &QToolButton::clicked, this, [this] { runSearch(); });

  connect(view_, &QTableView::doubleClicked, this,
          [this](const QModelIndex&) { openSelected(false); });
  connect(view_, &QTableView::customContextMenuRequested, this,
          [&](const QPoint& pos) {
            QMenu menu(this);
            menu.addAction(tr("打开"), this, [this] { openSelected(false); });
            menu.addAction(tr("打开所在文件夹"), this,
                           [this] { openSelected(true); });
            menu.addSeparator();
            menu.addAction(tr("复制路径"), this, [this] { copyPaths(); });
            menu.addAction(tr("删除到回收站"), this,
                           [this] { trashSelected(); });
            menu.exec(view_->viewport()->mapToGlobal(pos));
          });

  // 键盘优先（学 Everything）
  auto* scOpen = new QShortcut(Qt::Key_Return, view_);
  connect(scOpen, &QShortcut::activated, this, [this] { openSelected(false); });
  auto* scFolder = new QShortcut(QKeySequence("Ctrl+Return"), view_);
  connect(scFolder, &QShortcut::activated, this, [this] { openSelected(true); });
  auto* scCopy = new QShortcut(QKeySequence::Copy, view_);
  connect(scCopy, &QShortcut::activated, this, [this] { copyPaths(); });
  auto* scTrash = new QShortcut(QKeySequence(Qt::Key_Delete), view_);
  connect(scTrash, &QShortcut::activated, this, [this] { trashSelected(); });

  // 列排序（表头点击）
  view_->horizontalHeader()->setSectionsClickable(true);
  connect(view_->horizontalHeader(), &QHeaderView::sectionClicked, this,
          [this](int col) {
            if (col == sortCol_)
              sortOrder_ = (sortOrder_ == Qt::AscendingOrder)
                               ? Qt::DescendingOrder
                               : Qt::AscendingOrder;
            else {
              sortCol_ = col;
              sortOrder_ = Qt::AscendingOrder;
            }
            view_->horizontalHeader()->setSortIndicator(sortCol_, sortOrder_);
            model_->applySort(sortCol_, sortOrder_);
            updateFacets();
          });
}

void MainWindow::setupTray() {
  // 无托盘环境静默跳过（自动化测试可用 KYSEARCH_TRAY_FORCE=1 强制构建）
  if (!qEnvironmentVariableIsSet("KYSEARCH_TRAY_FORCE") &&
      !QSystemTrayIcon::isSystemTrayAvailable())
    return;
  tray_ = new QSystemTrayIcon(QIcon::fromTheme("system-search"), this);
  tray_->setToolTip(tr("麒麟快搜 · KySearch（Ctrl+Alt+K 唤起）"));
  trayMenu_ = new QMenu(this);

  // 状态行（禁用态；aboutToShow 时与状态栏同步）
  trayStatusAct_ = trayMenu_->addAction(tr("麒麟快搜"));
  trayStatusAct_->setEnabled(false);
  trayMenu_->addSeparator();

  trayMenu_->addAction(QIcon::fromTheme("edit-find"),
                       tr("显示搜索窗口\tCtrl+Alt+K"), this, [this] {
                         show();
                         raise();
                         activateWindow();
                         searchEdit_->setFocus();
                         searchEdit_->selectAll();
                       });

  trayHistoryMenu_ =
      trayMenu_->addMenu(QIcon::fromTheme("document-open-recent"), tr("最近搜索"));

  // 快捷过滤：直接下发过滤器语法，点击即搜
  auto* presetMenu = trayMenu_->addMenu(QIcon::fromTheme("system-search"),
                                        tr("快捷过滤"));
  const QVector<QPair<QString, QString>> presets = {
      {tr("今天修改"), QStringLiteral("dm:today")},
      {tr("最近 7 天"), QStringLiteral("dm:7d")},
      {tr("大文件（>100MB）"), QStringLiteral("size:>100mb")},
      {tr("文档"), QStringLiteral(
           "ext:doc,docx,pdf,ppt,pptx,xls,xlsx,txt,md,wps,et,dps")},
      {tr("图片"), QStringLiteral("ext:jpg,jpeg,png,gif,bmp,webp,svg,tiff,heic")},
      {tr("视频"), QStringLiteral("ext:mp4,mkv,avi,mov,wmv,flv,ts,webm")},
      {tr("仅文件夹"), QStringLiteral("type:dir")},
      {tr("最近下载"), QStringLiteral("path:") +
                           QFileInfo(QStandardPaths::writableLocation(
                                         QStandardPaths::DownloadLocation))
                               .fileName()
                               .toLower()},
  };
  for (const auto& p : presets)
    presetMenu->addAction(p.first, this,
                          [this, q = p.second] { applyTrayPreset(q); });

  trayMenu_->addSeparator();

  trayMenu_->addAction(QIcon::fromTheme("view-refresh"), tr("重建索引"), this,
                       [this] { rebuildIndex(); });

  trayMenu_->addAction(QIcon::fromTheme("utilities-system-monitor"),
                       tr("守护进程状态…"), this, [this] { showDaemonStatus(); });

  trayThemeMenu_ =
      trayMenu_->addMenu(QIcon::fromTheme("preferences-desktop-theme"),
                         tr("主题"));
  const QStringList themeNames = {tr("跟随系统"), tr("浅色"), tr("深色")};
  for (int i = 0; i < themeNames.size(); ++i) {
    QAction* act = trayThemeMenu_->addAction(themeNames[i]);
    act->setCheckable(true);
    act->setData(i);
    const int mode = i;
    connect(act, &QAction::triggered, this, [this, mode] {
      applyTheme(mode);
      QSettings s;
      s.setValue("theme", mode);
    });
  }

  // 开机自启：界面（desktop 文件）+ 后台索引服务（systemd 用户服务）
  auto* autoMenu = trayMenu_->addMenu(QIcon::fromTheme("preferences-system-login"),
                                      tr("开机自启"));
  trayAutoGuiAct_ = autoMenu->addAction(tr("搜索界面"), this, [this] {
    set_gui_autostart(trayAutoGuiAct_->isChecked());
    leftStatus_->setText(trayAutoGuiAct_->isChecked()
                             ? tr("✓ 已开机自启：搜索界面")
                             : tr("○ 已取消开机自启：搜索界面"));
  });
  trayAutoGuiAct_->setCheckable(true);
  trayAutoDaemonAct_ = autoMenu->addAction(tr("后台索引守护进程"), this, [this] {
    const bool on = trayAutoDaemonAct_->isChecked();
    const int rc = set_daemon_autostart(on);
    if (rc != 0)
      leftStatus_->setText(tr("✗ 切换守护进程自启失败（rc=%1，"
                              "需以 systemd 用户服务方式安装 kysearchd）").arg(rc));
    else
      leftStatus_->setText(on ? tr("✓ 已开机自启：后台索引守护进程")
                              : tr("○ 已取消开机自启：后台索引守护进程"));
  });
  trayAutoDaemonAct_->setCheckable(true);

  trayMenu_->addAction(QIcon::fromTheme("preferences-system"), tr("设置…"),
                       this, [this] { openSettings(); });
  trayMenu_->addSeparator();
  trayMenu_->addAction(QIcon::fromTheme("application-exit"), tr("退出"), this,
                       [this] {
                         forceQuit_ = true;
                         close();
                       });

  tray_->setContextMenu(trayMenu_);
  connect(trayMenu_, &QMenu::aboutToShow, this, &MainWindow::refreshTrayMenu);

  // 单击 = 显示并聚焦（不 toggle，避免误触隐藏）；双击保持原 toggle 行为
  connect(tray_, &QSystemTrayIcon::activated, this,
          [this](QSystemTrayIcon::ActivationReason r) {
            if (r == QSystemTrayIcon::Trigger) {
              show();
              raise();
              activateWindow();
              searchEdit_->setFocus();
              searchEdit_->selectAll();
            } else if (r == QSystemTrayIcon::DoubleClick) {
              onHotkeyActivated();
            }
          });

  // 状态栏文本低频轮询同步到气泡（状态栏无变更信号，5s 足够）
  auto* tipTimer = new QTimer(this);
  connect(tipTimer, &QTimer::timeout, this, &MainWindow::refreshTrayTooltip);
  tipTimer->start(5000);

  tray_->show();
  refreshTrayTooltip();

  // 自动化验证钩子：打印菜单结构（含动态刷新后的最近搜索）+ 可选渲染截图
  if (qEnvironmentVariableIsSet("KYSEARCH_TRAY_DEBUG")) {
    refreshTrayMenu();
    std::fprintf(stderr, "[tray] tooltip: %s\n",
                 tray_->toolTip().toUtf8().constData());
    std::function<void(QMenu*, int)> dump = [&](QMenu* m, int depth) {
      for (QAction* a : m->actions()) {
        if (a->isSeparator()) {
          std::fprintf(stderr, "%*s----\n", depth * 2, "");
          continue;
        }
        std::fprintf(stderr, "%*s%s%s\n", depth * 2, "",
                     a->text().toUtf8().constData(),
                     a->menu() ? " >" : (a->isEnabled() ? "" : " (disabled)"));
        if (a->menu()) dump(a->menu(), depth + 1);
      }
    };
    dump(trayMenu_, 1);
    if (const char* shot = std::getenv("KYSEARCH_TRAY_SHOT")) {
      trayMenu_->resize(trayMenu_->sizeHint());
      trayMenu_->grab().save(QString::fromLocal8Bit(shot));
    }
  }
}

void MainWindow::refreshTrayMenu() {
  // 状态行复用状态栏文本（远程/本地双模式均实时）
  const QString st = leftStatus_->text();
  trayStatusAct_->setText(st.isEmpty() ? tr("麒麟快搜 · KySearch") : st);

  // 最近搜索：最近 5 条，点击开窗即搜
  trayHistoryMenu_->clear();
  const QStringList hist = historyModel_->stringList().mid(0, 5);
  if (hist.isEmpty()) {
    QAction* a = trayHistoryMenu_->addAction(tr("（暂无记录）"));
    a->setEnabled(false);
  } else {
    for (const QString& h : hist)
      trayHistoryMenu_->addAction(h, this,
                                  [this, q = h] { applyTrayPreset(q); });
  }

  for (QAction* a : trayThemeMenu_->actions())
    a->setChecked(a->data().toInt() == curTheme_);

  // 开机自启勾选与实际状态同步（每次打开即时反映）
  trayAutoGuiAct_->setChecked(QFile::exists(gui_autostart_file()));
  trayAutoDaemonAct_->setChecked(daemon_autostart_enabled());
  refreshTrayTooltip();
}

// 守护进程状态对话框：独立短连接查询 STATUS，不影响主窗口远程会话
void MainWindow::showDaemonStatus() {
  QDialog dlg(this);
  dlg.setWindowTitle(tr("守护进程状态"));
  auto* txt = new QPlainTextEdit(&dlg);
  txt->setReadOnly(true);
  auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
  connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  auto* lay = new QVBoxLayout(&dlg);
  lay->addWidget(txt, 1);
  lay->addWidget(bb, 0, Qt::AlignRight);
  dlg.resize(560, 340);

  QString line;
  QLocalSocket s;
  s.connectToServer(opt_.socket);
  if (s.waitForConnected(1000)) {
    s.write("STATUS\n");
    s.waitForBytesWritten(500);
    for (int i = 0; i < 20 && s.state() == QLocalSocket::ConnectedState; ++i) {
      if (!s.waitForReadyRead(200)) break;
      line += QString::fromUtf8(s.readAll());
      if (line.contains('\n')) break;
    }
  }
  QString body;
  if (line.startsWith(QStringLiteral("OK "))) {
    QHash<QString, QString> kv;
    for (const QString& p : line.mid(3).trimmed().split(' ')) {
      const auto e = p.split('=');
      if (e.size() == 2) kv[e[0]] = e[1];
    }
    const QString be = kv.value(QStringLiteral("backend"));
    QString beName;
    if (be == QStringLiteral("fanotify"))
      beName = tr("fanotify（文件系统级实时，最优）");
    else if (be == QStringLiteral("inotify"))
      beName = tr("inotify（目录级实时）");
    else if (be == QStringLiteral("rescan"))
      beName = tr("定时对账（降级模式）");
    else
      beName = be;
    const qint64 up = kv.value(QStringLiteral("uptime")).toLongLong();
    QString upStr;
    if (up >= 86400) upStr = tr("%1 天 %2 小时").arg(up / 86400).arg(up % 86400 / 3600);
    else if (up >= 3600) upStr = tr("%1 小时 %2 分").arg(up / 3600).arg(up % 3600 / 60);
    else if (up >= 60) upStr = tr("%1 分 %2 秒").arg(up / 60).arg(up % 60);
    else upStr = tr("%1 秒").arg(up);
    body = tr("状态：● 运行中\n") +
           tr("连接：%1\n").arg(opt_.socket) +
           tr("索引条目：%1\n").arg(kv.value(QStringLiteral("entries"))) +
           tr("实时监控：%1\n").arg(beName) +
           tr("运行时长：%1\n").arg(upStr) +
           tr("索引根目录：\n  %1")
               .arg(QString(kv.value(QStringLiteral("roots")))
                        .replace(',', tr("\n  ")));
  } else {
    body = tr("✗ 守护进程未运行或无法连接\n\n连接：%1\n\n"
              "启动方式（任选其一）：\n"
              "  systemctl --user enable --now kysearch\n"
              "  kysearchd --config ~/.config/kysearch/kysearchd.conf")
               .arg(opt_.socket);
    if (!opt_.remote)
      body += tr("\n\n提示：当前界面为本地快照模式，守护进程可提供实时索引。");
  }
  txt->setPlainText(body);
  dlg.exec();
}

void MainWindow::refreshTrayTooltip() {
  if (!tray_) return;
  QString tip = tr("麒麟快搜 · KySearch（Ctrl+Alt+K 唤起）");
  const QString st = leftStatus_->text();
  if (!st.isEmpty()) tip += '\n' + st;
  tray_->setToolTip(tip);
  if (qEnvironmentVariableIsSet("KYSEARCH_TRAY_DEBUG"))
    std::fprintf(stderr, "[tray] tip refresh: %s\n",
                 tip.replace('\n', " | ").toUtf8().constData());
}

void MainWindow::applyTrayPreset(const QString& query) {
  show();
  raise();
  activateWindow();
  setQueryForTest(query);  // 设置查询词并立即执行（不等防抖）
  searchEdit_->setFocus();
}

void MainWindow::setupHotkey() {
  hotkey_ = new GlobalHotkey(this);
  connect(hotkey_, &GlobalHotkey::activated, this,
          &MainWindow::onHotkeyActivated, Qt::QueuedConnection);
  connect(hotkey_, &GlobalHotkey::failed, this, &MainWindow::onHotkeyFailed,
          Qt::QueuedConnection);
  hotkey_->start();
}

void MainWindow::onHotkeyActivated() {
  if (qEnvironmentVariableIsSet("KYSEARCH_HOTKEY_DEBUG"))
    std::fprintf(stderr, "[hotkey] activated, visible=%d -> %d\n", isVisible(),
                 !isVisible());
  if (isVisible()) {
    hide();
  } else {
    show();
    raise();
    activateWindow();
    searchEdit_->setFocus();
    searchEdit_->selectAll();
  }
}

void MainWindow::onHotkeyFailed(const QString& reason) {
  leftStatus_->setText(tr("○ 热键不可用：%1").arg(reason));
}

void MainWindow::applyTheme(int mode) {
  curTheme_ = mode;  // 托盘主题菜单勾选状态
  if (mode == 2) {  // 深色
    QPalette p;
    const QColor window(37, 37, 40), base(30, 30, 33), text(232, 232, 232);
    const QColor dim(150, 150, 154), hl(48, 92, 160), btn(48, 48, 52);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, QColor(38, 38, 41));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, btn);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, hl);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, window);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, dim);
    p.setColor(QPalette::Disabled, QPalette::Text, dim);
    QApplication::setPalette(p);
  } else {
    QApplication::setPalette(defaultPalette_);  // 浅色 / 跟随系统
  }
}

void MainWindow::saveHistory(const QString& text) {
  if (text.trimmed().isEmpty()) return;
  QStringList list = historyModel_->stringList();
  list.removeAll(text);
  list.prepend(text);
  while (list.size() > 20) list.removeLast();
  historyModel_->setStringList(list);
  QSettings st;
  st.setValue("history", list);
}

void MainWindow::onReturnPressed() { saveHistory(searchEdit_->text()); }

// ---------------- 本地模式 ----------------

void MainWindow::tryLoadDbOrScan() {
  if (QFile::exists(opt_.dbPath) &&
      kysearch::FileIndex::load(opt_.dbPath.toStdString(), index_)) {
    model_->setIndex(&index_);
    leftStatus_->setText(
        tr("● 索引就绪：%1 项（快照模式；启动 kysearchd 可获得实时索引）")
            .arg((qulonglong)index_.alive_count()));
    runSearch();
    return;
  }
  startScan();
}

// 重建索引：本地模式串行收尾在途线程后清空重扫；远程模式转 RESCAN
void MainWindow::rebuildIndex() {
  if (opt_.remote) {
    if (sock_ && sock_->state() == QLocalSocket::ConnectedState) {
      sock_->write("RESCAN\n");
      leftStatus_->setText(tr("⟳ 已请求守护进程重建索引…"));
    } else {
      leftStatus_->setText(tr("✗ 守护进程未连接，无法重建索引"));
    }
    return;
  }
  ++searchGen_;  // 作废在途异步搜索
  if (searchThread_.joinable()) searchThread_.join();
  // 上一轮扫描若仍在跑必须先收尾：对 joinable 线程直接赋值会
  // std::terminate；且 scan 线程正在写 index_，clear() 会与之竞争
  if (scanThread_.joinable()) scanThread_.join();
  model_->setResults(std::vector<kysearch::u32>{});  // 旧下标对新索引无效
  index_.clear();
  startScan();
}

void MainWindow::startScan() {
  if (scanThread_.joinable()) scanThread_.join();  // 防重入（std::terminate）
  scanCancel_ = false;
  searchEdit_->setEnabled(false);
  leftStatus_->setText(tr("⟳ 正在建立索引…"));
  scanThread_ = std::thread([this] {
    kysearch::ScanOptions sopt;
    sopt.roots = {opt_.root.toStdString()};
    sopt.pinyin_fn = kysearch::pinyin_key;
    sopt.cancel = &scanCancel_;  // 退出时尽快收尾，避免 join 挂住进程
    sopt.progress = [this](size_t f, size_t d) {
      if (shuttingDown_) return;
      QMetaObject::invokeMethod(this, "onScanProgress", Qt::QueuedConnection,
                                Q_ARG(qulonglong, (qulonglong)f),
                                Q_ARG(qulonglong, (qulonglong)d));
    };
    kysearch::Scanner sc(index_);
    sc.run(sopt);
    if (shuttingDown_) return;
    index_.save(opt_.dbPath.toStdString());
    QMetaObject::invokeMethod(this, "onScanFinished", Qt::QueuedConnection);
  });
}

void MainWindow::onScanProgress(qulonglong files, qulonglong dirs) {
  leftStatus_->setText(tr("⟳ 正在建立索引… 已索引 %1 个文件、%2 个目录")
                           .arg(files)
                           .arg(dirs));
}

void MainWindow::onScanFinished() {
  model_->setIndex(&index_);
  leftStatus_->setText(
      tr("● 索引就绪：%1 项（快照模式；启动 kysearchd 可获得实时索引）")
          .arg((qulonglong)index_.alive_count()));
  searchEdit_->setEnabled(true);
  searchEdit_->setFocus();
  runSearch();
}

void MainWindow::setQueryForTest(const QString& text) {
  searchEdit_->setText(text);
  runSearch();
}

void MainWindow::selectRowForTest(int row) {
  debounce_->stop();
  pendingSelectRow_ = row;  // 结果异步送达后应用
  if (model_ && row >= 0 && row < model_->rowCount()) {
    view_->selectRow(row);
    view_->setFocus();
    pendingSelectRow_ = -1;
  }
}

// 搜索框文本 + 侧栏类型过滤合成最终查询
QString MainWindow::effectiveQuery() const {
  QString t = searchEdit_->text().trimmed();
  if (!typeFilter_.isEmpty() && !t.contains(typeFilter_)) t += ' ' + typeFilter_;
  return t;
}

void MainWindow::onDebouncedSearch() {
  if (!searchEdit_->isEnabled()) return;
  runSearch();
}

void MainWindow::runSearch() {
  if (opt_.remote) {
    runRemoteSearch();
    return;
  }
  if (!searchEdit_->isEnabled() || !index_.finalized()) return;
  kysearch::QueryFlags flags;
  flags.match_path = pathBtn_->isChecked();
  flags.case_sensitive = caseBtn_->isChecked();
  flags.whole_word = wordBtn_->isChecked();
  flags.regex = regexBtn_->isChecked();
  const std::string text = effectiveQuery().toStdString();
  const QString qtext = QString::fromStdString(text);
  const std::string cdir =
      opt_.dbPath.section('/', 0, -2).toStdString() + "/content.db";
  const int gen = ++searchGen_;
  // 异步执行（百万条目时打字不卡 UI）；上一轮 worker 早已结束或毫秒级收尾
  if (searchThread_.joinable()) searchThread_.join();
  searchThread_ = std::thread([this, gen, flags, text, cdir, qtext] {
    auto q = kysearch::parse_query(text, flags);
    QElapsedTimer t;
    t.start();
    std::vector<kysearch::u32> res;
    index_.search(q, res);
#ifdef KYSEARCH_HAVE_SQLITE
    if (!q.content_terms.empty()) {
      kysearch::TextIndex ti;
      if (ti.open(cdir))
        kysearch::apply_content_filter(ti, q, index_, res);
      else
        res.clear();
    }
#endif
    const qint64 ms = t.elapsed();
    QMetaObject::invokeMethod(
        this,
        [this, gen, ms, qtext, res = std::move(res)]() mutable {
          if (gen != searchGen_.load()) return;  // 过期结果丢弃
          model_->setResults(std::move(res));
          model_->applySort(sortCol_, sortOrder_);
          rightStatus_->setText(
              tr("命中 %1 · %2 ms").arg(model_->rowCount()).arg(ms));
          if (qEnvironmentVariableIsSet("KYSEARCH_GUI_DEBUG"))
            std::fprintf(stderr, "[gui] query='%s' hits=%d\n",
                         qtext.toUtf8().constData(), model_->rowCount());
          updateFacets();
          if (pendingSelectRow_ >= 0 && pendingSelectRow_ < model_->rowCount()) {
            view_->selectRow(pendingSelectRow_);
            view_->setFocus();
            pendingSelectRow_ = -1;
            if (qEnvironmentVariableIsSet("KYSEARCH_SEL_DEBUG"))
              std::fprintf(stderr, "[sel] applied, has=%d rows=%d\n",
                           (int)view_->selectionModel()->hasSelection(),
                           model_->rowCount());
          }
        },
        Qt::QueuedConnection);
  });
}

// ---------------- 远程模式（kysearchd）----------------

void MainWindow::runRemoteSearch() {
  if (!sock_ || sock_->state() != QLocalSocket::ConnectedState) return;
  QString f;
  if (pathBtn_->isChecked()) f += 'p';
  if (caseBtn_->isChecked()) f += 'c';
  if (wordBtn_->isChecked()) f += 'w';
  if (regexBtn_->isChecked()) f += 'r';
  const QString cmd = QString("SEARCH\t%1\t0\t%2\t%3\n")
                          .arg(f.isEmpty() ? "-" : f)
                          .arg(5000)
                          .arg(effectiveQuery());
  remoteRows_.clear();
  remoteTimer_.start();
  sock_->write(cmd.toUtf8());
}

void MainWindow::onRemoteConnected() {
  if (reconnectTimer_) reconnectTimer_->stop();
  sock_->write("STATUS\n");  // 拿后端/条目信息展示；后续查询独立解析
  if (!searchEdit_->text().isEmpty()) runRemoteSearch();
}

void MainWindow::onRemoteDisconnected() {
  leftStatus_->setText(tr("✗ 守护进程已断开（自动重连中…）"));
  if (reconnectTimer_) reconnectTimer_->start();
}

void MainWindow::onRemoteReadyRead() {
  // 字节级累积 + 完整行再解码：按块解码会在 TCP 读边界切断多字节
  // UTF-8 字符（中文文件名变 U+FFFD 且不可恢复）；上限防服务端异常刷内存
  sockBuf_ += sock_->readAll();
  if (sockBuf_.size() > 64 * 1024 * 1024) {
    sockBuf_.clear();
    leftStatus_->setText(tr("✗ 守护进程响应异常（超限已丢弃）"));
    return;
  }
  int nl;
  while ((nl = sockBuf_.indexOf('\n')) >= 0) {
    QString line = QString::fromUtf8(sockBuf_.constData(), nl);
    sockBuf_.remove(0, nl + 1);
    if (line.endsWith('\r')) line.chop(1);

    if (line.startsWith("OK ")) {  // STATUS 或 SEARCH 的 OK 行
      const QString rest = line.mid(3);
      if (rest.contains("backend=")) {
        QHash<QString, QString> kv;
        for (const auto& p : rest.split(' ')) {
          auto e = p.split('=');
          if (e.size() == 2) kv[e[0]] = e[1];
        }
        leftStatus_->setText(
            tr("● 守护进程模式：%1 条目 · 监控 %2（实时索引）")
                .arg(kv.value("entries"))
                .arg(kv.value("backend")));
      }
      continue;  // SEARCH 的 OK count 行无需处理（以 END 汇总）
    }
    if (line == "END") {
      model_->setRemoteResults(std::move(remoteRows_));
      remoteRows_.clear();
      model_->applySort(sortCol_, sortOrder_);
      rightStatus_->setText(tr("命中 %1 · %2 ms · 实时索引")
                                .arg(model_->rowCount())
                                .arg(remoteTimer_.elapsed()));
      if (qEnvironmentVariableIsSet("KYSEARCH_GUI_DEBUG"))
        std::fprintf(stderr, "[gui] remote query='%s' hits=%d\n",
                     effectiveQuery().toUtf8().constData(), model_->rowCount());
      updateFacets();
      continue;
    }
    if (line.startsWith("ERR")) {
      rightStatus_->setText(line);
      continue;
    }
    // 结果行：F|D<TAB>size<TAB>mtime_sec<TAB>path
    auto parts = line.split('\t');
    if (parts.size() == 4) {
      ResultModel::RemoteRow row;
      row.type = parts[0];
      // 保留原始字节数排序（原存格式化串"1.2 MB"导致 toLongLong 只取到 1）
      row.sizeBytes = parts[1].toLongLong();
      row.size = parts[0] == "D" ? "-" : human_size_q(row.sizeBytes);
      row.mtime = QDateTime::fromSecsSinceEpoch(parts[2].toLongLong())
                      .toString("yyyy-MM-dd hh:mm");
      row.path = parts[3];
      row.name = parts[3].section('/', -1);
      remoteRows_.push_back(std::move(row));
    }
  }
}

// ---------------- 通用操作 ----------------

// 打开父目录并选中该文件（Everything /select 同款体验）。
// 走 freedesktop FileManager1.ShowItems（麒麟 peony 支持）；无该服务时退回普通打开目录。
void MainWindow::RevealInFileManager(const QString& path, bool& revealed) {
  revealed = false;
#ifdef KYSEARCH_HAVE_QTDBUS
  QDBusInterface fm(QStringLiteral("org.freedesktop.FileManager1"),
                    QStringLiteral("/org/freedesktop/FileManager1"),
                    QStringLiteral("org.freedesktop.FileManager1"));
  if (fm.isValid()) {
    QDBusReply<void> r = fm.call(
        QStringLiteral("ShowItems"),
        QStringList(QUrl::fromLocalFile(path).toString()),
        QStringLiteral(""));
    revealed = r.isValid();
  }
#endif
  if (!revealed)
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

void MainWindow::openSelected(bool openFolder) {
  auto rows = view_->selectionModel()->selectedRows(0);
  if (rows.isEmpty() && model_->rowCount() > 0)
    rows << model_->index(0, 0);
  for (const auto& idx : rows) {
    QString path = model_->rowPath(idx.row());
    if (openFolder) {
      bool revealed = false;
      RevealInFileManager(path, revealed);
    } else {
      QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }
  }
}

void MainWindow::copyPaths() {
  QStringList paths;
  for (const auto& idx : view_->selectionModel()->selectedRows(0))
    paths << model_->rowPath(idx.row());
  if (!paths.isEmpty())
    QApplication::clipboard()->setText(paths.join(QLatin1Char('\n')));
}

void MainWindow::trashSelected() {
  auto rows = view_->selectionModel()->selectedRows(0);
  if (rows.isEmpty()) return;
  int done = 0;
  for (const auto& idx : rows) {
    QString path = model_->rowPath(idx.row());
    std::string err;
    if (kysearch::trash_move(path.toStdString(), &err)) {
      ++done;
      if (!opt_.remote) index_.remove_by_path(path.toStdString());
    } else {
      leftStatus_->setText(tr("删除失败：%1").arg(QString::fromStdString(err)));
    }
  }
  leftStatus_->setText(tr("已移入回收站 %1 项").arg(done));
  if (opt_.remote) {  // 守护进程监控自动同步，稍候刷新
    QTimer::singleShot(1200, this, [this] { runSearch(); });
  } else {
    runSearch();
  }
}

void MainWindow::updateFacets() {
  if (!facetsDock_ || !facetsDock_->isVisible()) return;
  const int n = model_->rowCount();
  facetList_->clear();
  if (n > 50000) {  // 超大结果集跳过聚合并提示
    facetList_->addItem(tr("结果超过 5 万项，已跳过聚合"));
    return;
  }
  QHash<QString, int> exts, dirs;
  int files = 0, dirs_cnt = 0;
  if (model_->remote()) {
    for (const auto& r : model_->remoteRows()) {
      if (r.type == "D") ++dirs_cnt;
      else ++files;
      QString name = r.name;
      int dot = name.lastIndexOf('.');
      if (dot >= 0 && dot + 1 < name.size())
        exts[name.mid(dot + 1).toLower()]++;
      dirs[r.path.section('/', 0, -2)]++;
    }
  } else {
    for (auto i : model_->localRows()) {
      const auto& e = index_.entry(i);
      if (e.type == kysearch::EntryType::Dir) ++dirs_cnt;
      else ++files;
      auto sv = index_.lname(i);
      QString nm = QString::fromUtf8(sv.data(), (int)sv.size());
      int dot = nm.lastIndexOf('.');
      if (dot >= 0 && dot + 1 < nm.size()) exts[nm.mid(dot + 1)]++;
      auto pv = index_.opath(i);
      QString fp = QString::fromUtf8(pv.data(), (int)pv.size());
      dirs[fp.section('/', 0, -2)]++;
    }
  }
  auto addSection = [this](const QString& title) {
    auto* h = new QListWidgetItem(title, facetList_);
    h->setFlags(Qt::NoItemFlags);
    facetList_->addItem(h);
  };
  auto addItems = [this](const QString& prefix, const QHash<QString, int>& m,
                         int top) {
    QVector<QPair<int, QString>> v;
    for (auto it = m.begin(); it != m.end(); ++it) v.append({it.value(), it.key()});
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (int i = 0; i < v.size() && i < top; ++i) {
      auto syntax = prefix == "ext:" ? "ext:" + v[i].second
                                     : "path:" + v[i].second;
      auto* it = new QListWidgetItem(
          tr("%1 × %2").arg(v[i].second).arg(v[i].first), facetList_);
      it->setData(Qt::UserRole, syntax);
      facetList_->addItem(it);
    }
  };
  addSection(tr("类型"));
  auto* fItem = new QListWidgetItem(tr("文件 × %1").arg(files), facetList_);
  fItem->setData(Qt::UserRole, "type:file");
  facetList_->addItem(fItem);
  auto* dItem = new QListWidgetItem(tr("文件夹 × %1").arg(dirs_cnt), facetList_);
  dItem->setData(Qt::UserRole, "type:dir");
  facetList_->addItem(dItem);
  addSection(tr("扩展名 Top 12"));
  addItems("ext:", exts, 12);
  addSection(tr("目录 Top 12"));
  addItems("path:", dirs, 12);
}

void MainWindow::applyFacet(const QString& syntax) {
  if (syntax.isEmpty()) return;
  QString text = searchEdit_->text();
  if (text.contains(syntax)) return;  // 已应用
  searchEdit_->setText(text.isEmpty() ? syntax : text + " " + syntax);
}

void MainWindow::updatePreview() {
  if (!previewDock_) return;
  if (!previewDock_->isVisible()) return;  // 不可见时跳过（重开后由信号刷新）
  auto rows = view_->selectionModel()->selectedRows(0);
  if (rows.isEmpty()) {
    previewStack_->setCurrentWidget(previewInfo_);
    previewInfo_->setText(tr("选择一行查看预览"));
    return;
  }
  const QString path = model_->rowPath(rows.first().row());
  const QFileInfo fi(path);
  const QString ext = fi.suffix().toLower();
  static const char* kText[] = {"txt", "md",  "log", "csv", "json",
                                "xml", "html", "htm", "ini", "conf",
                                "yaml", "yml", "c",  "h",  "cpp"};
  static const char* kImg[] = {"png", "jpg", "jpeg", "bmp", "gif", "webp"};
  auto in_list = [&](const char* const* l, size_t n) {
    for (size_t i = 0; i < n; ++i)
      if (ext == l[i]) return true;
    return false;
  };
  if (in_list(kText, sizeof kText / sizeof kText[0]) && fi.size() <= 1024 * 1024) {
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
      QByteArray data = f.read(256 * 1024);
      previewText_->setPlainText(QString::fromUtf8(data));
      previewStack_->setCurrentWidget(previewText_);
      return;
    }
  } else if (in_list(kImg, sizeof kImg / sizeof kImg[0])) {
    QPixmap pm(path);
    if (!pm.isNull()) {
      previewImage_->setPixmap(
          pm.scaled(360, 360, Qt::KeepAspectRatio, Qt::SmoothTransformation));
      previewStack_->setCurrentWidget(previewImage_);
      return;
    }
  }
  previewInfo_->setText(tr("%1\n%2\n大小：%3\n修改：%4\n\n（此类型无预览，双击打开）")
                            .arg(fi.fileName())
                            .arg(fi.absolutePath())
                            .arg(human_size_q(fi.size()))
                            .arg(fi.lastModified().toString("yyyy-MM-dd hh:mm")));
  previewStack_->setCurrentWidget(previewInfo_);
}

void MainWindow::exportCsv() {
  const int n = model_->rowCount();
  if (n == 0) return;
  QString out = QFileDialog::getSaveFileName(
      this, tr("导出 CSV"), QDir::homePath() + "/kysearch.csv", "CSV (*.csv)");
  if (out.isEmpty()) return;
  QFile f(out);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
    leftStatus_->setText(tr("导出失败：%1").arg(out));
    return;
  }
  f.write("\"名称\",\"路径\",\"大小\",\"修改时间\",\"类型\"\n");
  QTextStream ts(&f);
  ts.setCodec("UTF-8");
  for (int r = 0; r < n; ++r) {
    QStringList cells;
    if (model_->remote()) {
      const auto& row = model_->remoteRows()[(size_t)r];
      cells << row.name << row.path.section('/', 0, -2) << row.size << row.mtime
            << (row.type == "D" ? tr("文件夹") : tr("文件"));
    } else {
      kysearch::u32 i = model_->localRows()[(size_t)r];
      const auto& e = index_.entry(i);
      QString p = model_->rowPath(r);
      cells << p.section('/', -1) << p.section('/', 0, -2)
            << human_size_q((qint64)e.size)
            << QDateTime::fromSecsSinceEpoch((qint64)e.mtime_sec)
                   .toString("yyyy-MM-dd hh:mm")
            << (e.type == kysearch::EntryType::Dir ? tr("文件夹")
                                                   : tr("文件"));
    }
    for (auto& c : cells) c.replace('"', "\"\"");
    ts << '\"' << cells.join("\",\"") << "\"\n";
  }
  leftStatus_->setText(tr("已导出 %1 行 -> %2").arg(n).arg(out));
}

void MainWindow::openSettings() {
  QDialog dlg(this);
  dlg.setWindowTitle(tr("设置"));
  auto* form = new QFormLayout(&dlg);
  // 根目录列表编辑器：添加（浏览）/移除，支持任意多个监控根
  auto* rootsList = new QListWidget(&dlg);
  rootsList->setSelectionMode(QAbstractItemView::SingleSelection);
  rootsList->setMaximumHeight(140);
  rootsList->setAlternatingRowColors(true);
  auto* addRootBtn = new QPushButton(tr("添加…"), &dlg);
  auto* delRootBtn = new QPushButton(tr("移除"), &dlg);
  delRootBtn->setEnabled(false);
  auto* rootsBtns = new QVBoxLayout();
  rootsBtns->addWidget(addRootBtn);
  rootsBtns->addWidget(delRootBtn);
  rootsBtns->addStretch(1);
  auto* rootsRow = new QWidget(&dlg);
  auto* rootsLay = new QHBoxLayout(rootsRow);
  rootsLay->setContentsMargins(0, 0, 0, 0);
  rootsLay->addWidget(rootsList, 1);
  rootsLay->addLayout(rootsBtns);
  auto* excludeEdit = new QLineEdit(&dlg);
  auto* contentEdit = new QLineEdit(&dlg);
  auto* autostartBox = new QCheckBox(tr("开机自动启动界面（托盘+热键）"), &dlg);
  QSettings st;
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
  const auto skipEmpty = Qt::SkipEmptyParts;
#else
  const auto skipEmpty = QString::SkipEmptyParts;  // focal/麒麟 Qt 5.12
#endif
  for (const QString& r : st.value("roots").toString().split(',', skipEmpty))
    rootsList->addItem(r);
  connect(addRootBtn, &QPushButton::clicked, this, [&] {
    const QString d = QFileDialog::getExistingDirectory(
        &dlg, tr("选择要索引的根目录"), QDir::homePath());
    if (d.isEmpty()) return;
    for (int i = 0; i < rootsList->count(); ++i)
      if (rootsList->item(i)->text() == d) return;  // 去重
    rootsList->addItem(d);
    rootsList->setCurrentRow(rootsList->count() - 1);
  });
  connect(rootsList, &QListWidget::itemSelectionChanged, this,
          [&] { delRootBtn->setEnabled(rootsList->currentRow() >= 0); });
  connect(delRootBtn, &QPushButton::clicked, this, [&] {
    const int row = rootsList->currentRow();
    if (row >= 0) delete rootsList->takeItem(row);
  });
  excludeEdit->setText(st.value("exclude").toString());
  contentEdit->setText(st.value("content_roots").toString());
  autostartBox->setChecked(QFile::exists(
      QDir::homePath() + "/.config/autostart/kysearch-gui.desktop"));
  form->addRow(tr("索引根目录（多个，全部实时监控）"), rootsRow);
  form->addRow(tr("排除目录名（逗号分隔）"), excludeEdit);
  form->addRow(tr("全文索引根（空=关闭）"), contentEdit);
  form->addRow("", autostartBox);
  auto* btns = new QDialogButtonBox(QDialogButtonBox::Save |
                                    QDialogButtonBox::Cancel, &dlg);
  form->addRow(btns);
  connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  auto* hint = new QLabel(
      tr("保存后生成 ~/.config/kysearch/kysearchd.conf；"
         "守护进程以 systemd 服务运行时自动重启生效，界面自动重连"), &dlg);
  hint->setWordWrap(true);
  form->addRow(hint);
  if (dlg.exec() != QDialog::Accepted) return;
  QStringList roots;
  for (int i = 0; i < rootsList->count(); ++i)
    roots << rootsList->item(i)->text();
  const QString rootsCsv = roots.join(',');
  st.setValue("roots", rootsCsv);
  st.setValue("exclude", excludeEdit->text());
  st.setValue("content_roots", contentEdit->text());
  // 写守护进程配置
  QDir().mkpath(QDir::homePath() + "/.config/kysearch");
  QFile cf(QDir::homePath() + "/.config/kysearch/kysearchd.conf");
  if (cf.open(QIODevice::WriteOnly | QIODevice::Text)) {
    QTextStream ts(&cf);
    ts.setCodec("UTF-8");
    if (!rootsCsv.isEmpty()) ts << "roots=" << rootsCsv << "\n";
    if (!excludeEdit->text().isEmpty())
      ts << "exclude=" << excludeEdit->text() << "\n";
    if (!contentEdit->text().isEmpty())
      ts << "content-roots=" << contentEdit->text() << "\n";
  }
  // 开机自启（与托盘菜单共用同一实现）
  set_gui_autostart(autostartBox->isChecked());
  // 远程模式：优先 SETROOTS 热生效（不重启守护进程、连接不断）
  if (opt_.remote && sock_ &&
      sock_->state() == QLocalSocket::ConnectedState && !rootsCsv.isEmpty()) {
    QString cmd = QStringLiteral("SETROOTS");
    for (const QString& r : roots) cmd += QLatin1Char('\t') + r;
    cmd += QLatin1Char('\n');  // 行协议必须以换行结束（否则守护进程永不处理）
    sock_->write(cmd.toUtf8());
    leftStatus_->setText(tr("✓ 索引根已实时生效（无需重启守护进程）"));
    return;
  }
  // 回退：守护进程以 systemd 用户服务运行时自动重启，配置即时生效
  QProcess systemctl;
  systemctl.start(QStringLiteral("systemctl"),
                  {QStringLiteral("--user"), QStringLiteral("is-active"),
                   QStringLiteral("kysearch")});
  if (systemctl.waitForFinished(3000) && systemctl.exitCode() == 0) {
    QProcess::startDetached(QStringLiteral("systemctl"),
                            {QStringLiteral("--user"), QStringLiteral("restart"),
                             QStringLiteral("kysearch")});
    leftStatus_->setText(tr("✓ 设置已保存，守护进程重启中（界面将自动重连）"));
  } else {
    leftStatus_->setText(
        tr("✓ 设置已保存（手动运行的守护进程需重启后生效）"));
  }
}

void MainWindow::closeEvent(QCloseEvent* ev) {
  if (tray_ && !forceQuit_) {
    // 收进托盘不是退出：不能置 shuttingDown_（否则重开后扫描进度回调
    // /远程重连全部静默失效）
    hide();
    ev->ignore();  // 收进托盘，保持热键可用
    return;
  }
  shuttingDown_ = true;
  scanCancel_ = true;  // 取消在途扫描：join 前先让扫描尽快收尾
  if (scanThread_.joinable()) scanThread_.join();
  QMainWindow::closeEvent(ev);
}
