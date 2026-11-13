// X11 全局热键（M2）：独立 Xlib 连接的抓键线程
// 抓 Ctrl+Alt+K 唤起/隐藏主窗口；Wayland 下不可用（自动跳过）。
#pragma once

#include <QThread>
#include <atomic>

class GlobalHotkey : public QThread {
  Q_OBJECT
 public:
  explicit GlobalHotkey(QObject* parent = nullptr);
  ~GlobalHotkey() override;

  void run() override;
  void stop() { stop_ = true; }

 signals:
  void activated();   // 热键按下（工作线程发出，队列送达接收方）
  void failed(const QString& reason);

 private:
  std::atomic<bool> stop_{false};  // 跨线程读写：原子量（原裸 bool 为数据竞争）
};
