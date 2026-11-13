// kysearch-gui 入口
// 常规：kysearch-gui [--db P] [--root P] [--theme 0|1|2]
// 远程：kysearch-gui --remote [--socket P]
// 自动化截图：--query TEXT --screenshot out.png
#include <QApplication>
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <cstdio>
#include <cstring>
#include <QTimer>
#include <QScreen>

#include "gui/mainwindow.h"

int main(int argc, char** argv) {
  // --reveal 轻量路径：无显示环境也能用（QCoreApplication 即可）
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--reveal") == 0) {
      QCoreApplication app(argc, argv);
      bool revealed = false;
      MainWindow::RevealInFileManager(QString::fromLocal8Bit(argv[i + 1]), revealed);
      std::printf("revealed=%d\n", (int)revealed);
      return 0;
    }
  }
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("kysearch"));
  QApplication::setApplicationVersion(QStringLiteral("0.2.1"));
  QApplication::setQuitOnLastWindowClosed(true);

  QCommandLineParser p;
  p.setApplicationDescription(QStringLiteral("麒麟快搜 · KySearch"));
  p.addHelpOption();
  QCommandLineOption dbOpt("db", "索引快照路径（默认 ~/.cache/kysearch/index.bin）", "path");
  QCommandLineOption rootOpt("root", "无快照时扫描的根目录（默认 $HOME）", "path");
  QCommandLineOption remoteOpt("remote", "连接 kysearchd 守护进程（实时索引）");
  QCommandLineOption socketOpt("socket", "守护进程 socket 路径", "path");
  QCommandLineOption themeOpt("theme", "主题：0 跟随系统 1 浅色 2 深色", "n");
  QCommandLineOption queryOpt("query", "初始搜索词", "text");
  QCommandLineOption shotOpt("screenshot", "渲染后截图保存并退出（自动化测试）", "path");
  QCommandLineOption selOpt("select-row", "搜索后选中第 N 行（0 起，配合预览截图）", "n");
  QCommandLineOption revealOpt("reveal", "在文件管理器中打开所在文件夹并选中该文件", "path");
  p.addOptions({dbOpt, rootOpt, remoteOpt, socketOpt, themeOpt, queryOpt, shotOpt, selOpt, revealOpt});
  p.process(app);

  MainWindow::Options mo;
  mo.dbPath = p.value(dbOpt);
  if (mo.dbPath.isEmpty()) {
    mo.dbPath = QDir::homePath() + QStringLiteral("/.cache/kysearch/index.bin");
    QDir().mkpath(QFileInfo(mo.dbPath).absolutePath());
  }
  mo.root = p.value(rootOpt).isEmpty() ? QDir::homePath() : p.value(rootOpt);
  mo.remote = p.isSet(remoteOpt);
  const QString xdg = qEnvironmentVariable("XDG_RUNTIME_DIR");
  mo.socket = p.value(socketOpt).isEmpty()
                  ? (xdg.isEmpty()
                         ? QDir::homePath() +
                               QStringLiteral("/.cache/kysearch/daemon.sock")
                         : xdg + QStringLiteral("/kysearch.sock"))
                  : p.value(socketOpt);
  mo.theme = p.value(themeOpt).isEmpty() ? 0 : p.value(themeOpt).toInt();
  mo.selectRow = p.value(selOpt).isEmpty() ? -1 : p.value(selOpt).toInt();

  MainWindow w(mo);
  if (!p.value(queryOpt).isEmpty()) w.setQueryForTest(p.value(queryOpt));
  w.show();
  if (mo.selectRow >= 0)
    w.selectRowForTest(mo.selectRow);  // 结果异步送达后应用选中

  // 自动化：打开指定对话框供截图（settings=设置页 / status=守护进程状态）
  const QString dlg = qEnvironmentVariable("KYSEARCH_GUI_DIALOG");
  if (!dlg.isEmpty())
    QTimer::singleShot(600, &w, [&w, dlg] {
      if (dlg == QLatin1String("settings")) w.openSettings();
      else if (dlg == QLatin1String("status")) w.showDaemonStatus();
    });

  if (!p.value(shotOpt).isEmpty()) {
    const QString out = p.value(shotOpt);
    QTimer::singleShot(700, &w, [&w, out, &app] {
      // 全屏抓取（Xvfb 下可含模态对话框）；无屏环境退回窗口抓取
      QPixmap pm;
      if (QScreen* sc = QApplication::primaryScreen()) pm = sc->grabWindow(0);
      if (pm.isNull()) pm = w.grab();
      pm.save(out);
      app.quit();
    });
  }
  return app.exec();
}
