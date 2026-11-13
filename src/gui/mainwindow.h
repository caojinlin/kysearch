// kysearch 主窗口（M2/M3 全功能版）
// 模式：
//   本地模式：内嵌引擎 + 快照（无守护进程时的独立运行）
//   远程模式：QLocalSocket 连 kysearchd，实时索引结果分页拉取
#pragma once

#include <QMainWindow>
#include <QElapsedTimer>
#include <atomic>
#include <thread>
#include <vector>

#include "core/index.h"

class QDockWidget;
class QListWidget;
class QPlainTextEdit;
class QStackedWidget;
#include "gui/resultmodel.h"

class QCompleter;
class QLabel;
class QLineEdit;
class QMenu;
class QAction;
class QStringListModel;
class QLocalSocket;
class QSystemTrayIcon;
class QTableView;
class QTimer;
class QToolButton;
class GlobalHotkey;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  struct Options {
    QString dbPath;
    QString root;
    QString socket;
    bool remote = false;   // 连接守护进程模式
    int theme = 0;         // 0 跟随系统 1 浅色 2 深色
    int selectRow = -1;    // 自动化测试：搜索后选中该行
  };

  explicit MainWindow(const Options& opt, QWidget* parent = nullptr);
  ~MainWindow() override;

  // 自动化测试用：设置查询词并立即执行（不等防抖）
  void setQueryForTest(const QString& text);
  bool hasResults() const { return model_ && model_->rowCount() > 0; }
  void selectRowForTest(int row);
  static void RevealInFileManager(const QString& path, bool& revealed);  // --reveal 与右键共用

 protected:
  void closeEvent(QCloseEvent* ev) override;

 private slots:
  void onDebouncedSearch();
  void onScanProgress(qulonglong files, qulonglong dirs);
  void onScanFinished();
  void onRemoteConnected();
  void onRemoteReadyRead();
  void onRemoteDisconnected();
  void onHotkeyActivated();
  void onHotkeyFailed(const QString& reason);
  void onReturnPressed();

 private:
  void buildUi();
  void rebuildIndex();       // 重建索引（本地重扫 / 远程 RESCAN）
  void tryLoadDbOrScan();
  void startScan();
  void runSearch();
  void runRemoteSearch();
  void openSelected(bool openFolder);
private:
  void revealInFileManager(const QString& path, bool& revealed);

public:
  void copyPaths();
  void saveHistory(const QString& text);
  void applyTheme(int mode);
  void setupTray();
  void setupHotkey();
  void trashSelected();
  void updateFacets();
  void applyFacet(const QString& syntax);
  void updatePreview();
  void exportCsv();
  void openSettings();
  void refreshTrayMenu();          // 托盘菜单打开前刷新（状态/历史/勾选）
  void refreshTrayTooltip();       // 索引状态变化同步到气泡提示
  void applyTrayPreset(const QString& query);  // 托盘预设/历史：开窗即搜
  void showDaemonStatus();         // 守护进程状态对话框（独立 STATUS 查询）
  QString effectiveQuery() const;  // 搜索框文本 + 工具栏开关（文件夹等）合成

  QTimer* reconnectTimer_ = nullptr;  // 远程模式守护进程重启期间自动重连

  QLineEdit* searchEdit_ = nullptr;
  QToolButton* pathBtn_ = nullptr;
  QToolButton* caseBtn_ = nullptr;
  QToolButton* wordBtn_ = nullptr;
  QToolButton* regexBtn_ = nullptr;
  QTableView* view_ = nullptr;
  ResultModel* model_ = nullptr;
  QLabel* leftStatus_ = nullptr;
  QLabel* rightStatus_ = nullptr;
  QTimer* debounce_ = nullptr;
  QCompleter* completer_ = nullptr;
  QStringListModel* historyModel_ = nullptr;

  // 托盘（右键菜单 + 状态联动）
  QMenu* trayMenu_ = nullptr;
  QMenu* trayHistoryMenu_ = nullptr;  // 最近搜索（aboutToShow 动态填充）
  QMenu* trayThemeMenu_ = nullptr;    // 主题（aboutToShow 刷新勾选）
  QAction* trayStatusAct_ = nullptr;  // 顶部状态行（禁用态）
  QAction* trayAutoGuiAct_ = nullptr;     // 开机自启：搜索界面
  QAction* trayAutoDaemonAct_ = nullptr;  // 开机自启：后台索引守护进程
  int curTheme_ = 0;                  // 当前主题模式（主题菜单勾选用）

  QLocalSocket* sock_ = nullptr;
  QByteArray sockBuf_;  // 原始字节累积：行完整后再解码 UTF-8（块边界可能切断多字节字符）
  std::vector<ResultModel::RemoteRow> remoteRows_;
  QElapsedTimer remoteTimer_;

  QSystemTrayIcon* tray_ = nullptr;
  GlobalHotkey* hotkey_ = nullptr;
  QPalette defaultPalette_;
  QDockWidget* facetsDock_ = nullptr;
  QListWidget* quickList_ = nullptr;  // 类型快速过滤（固定分类，单选）
  QListWidget* facetList_ = nullptr;  // 筛选分布（动态聚合，点击追加）
  QString typeFilter_;                // 当前类型过滤语法（空=全部）
  QDockWidget* previewDock_ = nullptr;
  QStackedWidget* previewStack_ = nullptr;
  QPlainTextEdit* previewText_ = nullptr;
  QLabel* previewImage_ = nullptr;
  QLabel* previewInfo_ = nullptr;
  int sortCol_ = 0;
  Qt::SortOrder sortOrder_ = Qt::AscendingOrder;
  std::thread searchThread_;
  std::atomic<bool> scanCancel_{false};  // 退出时取消在途扫描（防关不掉）
  std::atomic<int> searchGen_{0};
  int pendingSelectRow_ = -1;  // 结果送达后再应用选中（异步搜索竞态）

  kysearch::FileIndex index_;
  Options opt_;
  std::thread scanThread_;
  std::atomic<bool> shuttingDown_{false};
  bool forceQuit_ = false;
};
