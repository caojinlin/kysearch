#include "gui/hotkey.h"

#include <QMetaObject>
#include <QTimer>

#if defined(Q_OS_LINUX) && __has_include(<X11/Xlib.h>)
#define KYSEARCH_HAVE_X11 1
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#endif

GlobalHotkey::GlobalHotkey(QObject* parent) : QThread(parent) {}

GlobalHotkey::~GlobalHotkey() {
  stop_ = true;
  // 分段等待：超时仍存活则继续等（QThread 析构运行中的线程会崩溃，
  // 原一次性 wait(1500) 失败后直接析构属 UB）
  for (int i = 0; i < 30 && isRunning(); ++i) wait(500);
}

void GlobalHotkey::run() {
#if KYSEARCH_HAVE_X11
  Display* d = XOpenDisplay(nullptr);
  if (!d) {
    emit failed(QStringLiteral("无法打开 X 显示（Wayland 下全局热键不可用）"));
    return;
  }
  KeyCode kc = XKeysymToKeycode(d, XK_k);
  Window root = DefaultRootWindow(d);
  // 可检测自动重复：按住热键时只收到首次按下，避免切换语义高频闪烁
  XkbSetDetectableAutoRepeat(d, True, nullptr);
  unsigned int mod = ControlMask | Mod1Mask;  // Ctrl+Alt
  // NumLock / ScrollLock 修饰位变体都要抓，否则锁定键按下时热键失效
  const unsigned extras[] = {0, Mod2Mask, Mod5Mask, Mod2Mask | Mod5Mask};
  // XGrabKey 的 BadAccess 是异步上报的：走默认错误处理器会直接 exit
  // 整个进程。装临时处理器记录冲突，XSync 后恢复原处理器
  static volatile int grab_conflict = 0;
  grab_conflict = 0;
  auto grab_err = [](Display*, XErrorEvent*) -> int {
    grab_conflict = 1;
    return 0;
  };
  bool any = false;
  {
    XErrorHandler old_err = XSetErrorHandler(grab_err);
    for (unsigned ex : extras)
      XGrabKey(d, kc, mod | ex, root, False, GrabModeAsync, GrabModeAsync);
    XSync(d, False);
    XSetErrorHandler(old_err);
    any = kc != 0 && !grab_conflict;  // keycode 有效且无 BadAccess 冲突
  }
  if (getenv("KYSEARCH_HOTKEY_DEBUG"))
    fprintf(stderr, "[hotkey] grab keycode=%u conflict=%d root=%lu\n",
            (unsigned)kc, (int)grab_conflict, (unsigned long)root);
  if (!any) {
    emit failed(grab_conflict
                    ? QStringLiteral("热键 Ctrl+Alt+K 已被其他程序占用")
                    : QStringLiteral("XGrabKey 失败（keycode 无效）"));
    XCloseDisplay(d);
    return;
  }
  while (!stop_) {
    if (XPending(d)) {
      XEvent ev;
      XNextEvent(d, &ev);
      if (getenv("KYSEARCH_HOTKEY_DEBUG"))
        fprintf(stderr, "[hotkey] event type=%d key=%lu state=%lu\n",
                ev.type, (unsigned long)ev.xkey.keycode,
                (unsigned long)ev.xkey.state);
      if (ev.type == KeyPress &&
          (unsigned)ev.xkey.keycode == (unsigned)kc &&
          (ev.xkey.state & ControlMask) && (ev.xkey.state & Mod1Mask))
        emit activated();
    } else {
      usleep(100000);  // 100ms 轮询，保证 stop_ 可及时退出
    }
  }
  for (unsigned ex : extras) XUngrabKey(d, kc, mod | ex, root);
  XCloseDisplay(d);
#else
  emit failed(QStringLiteral("当前平台不支持全局热键"));
#endif
}
