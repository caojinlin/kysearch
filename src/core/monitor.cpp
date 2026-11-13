#include "core/monitor.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/fanotify.h>
// 兼容旧内核头（<5.9，如麒麟 V10 SP1 的 5.4）：fanotify DFID_NAME 宏缺失时补齐。
// 运行于旧内核时 fanotify_init 探测会失败并自动降级 inotify，行为安全。
#ifndef FAN_REPORT_DIR_FID
#define FAN_REPORT_DIR_FID 0x00000400
#endif
#ifndef FAN_REPORT_NAME
#define FAN_REPORT_NAME 0x00000800
#endif
#ifndef FAN_REPORT_DFID_NAME
#define FAN_REPORT_DFID_NAME (FAN_REPORT_DIR_FID | FAN_REPORT_NAME)
#endif
#ifndef FAN_EVENT_INFO_TYPE_DFID_NAME
#define FAN_EVENT_INFO_TYPE_DFID_NAME 2
#endif

#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <unordered_set>

#include "core/scanner.h"

namespace fs = std::filesystem;

namespace kysearch {

const char* monitor_level_name(MonitorLevel lv) {
  switch (lv) {
    case MonitorLevel::Fanotify: return "fanotify";
    case MonitorLevel::Inotify: return "inotify";
    case MonitorLevel::Rescan: return "rescan";
    default: return "none";
  }
}

namespace {

std::string errno_note(const char* what) {
  return std::string(what) + ": " + std::strerror(errno);
}

bool starts_with(const std::string& s, const std::string& p) {
  return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

Monitor::Monitor(FileIndex& idx, Options opt) : idx_(idx), opt_(std::move(opt)) {
  roots_snap_ =
      std::make_shared<const std::vector<std::string>>(opt_.roots);
}

Monitor::~Monitor() { stop(); }

void Monitor::start() {
  std::lock_guard<std::mutex> sg(stop_mtx_);  // 与 stop() 串行：防 fds/线程成员竞争
  if (event_thread_.joinable() || rescan_thread_.joinable()) return;
  stop_ = false;
  if (pipe2(stop_pipe_, O_NONBLOCK | O_CLOEXEC) != 0) stop_pipe_[0] = stop_pipe_[1] = -1;

  std::string note = "手动指定";
  bool want_fan = opt_.preferred == "auto" || opt_.preferred == "fanotify";
  bool want_ino = opt_.preferred == "auto" || opt_.preferred == "inotify";
  bool ok = false;
  if (want_fan && (ok = setup_fanotify(note))) level_ = MonitorLevel::Fanotify;
  else if (want_ino && (ok = setup_inotify(note))) level_ = MonitorLevel::Inotify;

  if (level_ == MonitorLevel::None) {
    level_ = MonitorLevel::Rescan;  // 无事件能力，仅靠对账
    note = "fanotify 与 inotify 均不可用，仅定时对账（" + note + "）";
  }
  if (opt_.on_level) opt_.on_level(level_.load(), note);
  if (level_ == MonitorLevel::Fanotify || level_ == MonitorLevel::Inotify)
    event_thread_ = std::thread(&Monitor::run_event_loop, this);
  rescan_thread_ = std::thread(&Monitor::run_rescan, this);
}

void Monitor::stop() {
  // 串行化整个停止流程：并发/重复调用 stop() 时两路同时 join 同一
  // std::thread 属未定义行为
  std::lock_guard<std::mutex> sg(stop_mtx_);
  if (stop_.exchange(true)) {
    // 已在停止流程：等线程退出
    if (event_thread_.joinable()) event_thread_.join();
    if (rescan_thread_.joinable()) rescan_thread_.join();
    return;
  }
  if (stop_pipe_[1] >= 0) {
    char c = 1;
    ssize_t rc = write(stop_pipe_[1], &c, 1);
    (void)rc;
  }
  if (event_thread_.joinable()) event_thread_.join();
  if (rescan_thread_.joinable()) rescan_thread_.join();
  if (fan_fd_ >= 0) close(fan_fd_), fan_fd_ = -1;
  if (inotify_fd_ >= 0) close(inotify_fd_), inotify_fd_ = -1;
  if (mount_fd_ >= 0) close(mount_fd_), mount_fd_ = -1;
  if (stop_pipe_[0] >= 0) close(stop_pipe_[0]);
  if (stop_pipe_[1] >= 0) close(stop_pipe_[1]);
  stop_pipe_[0] = stop_pipe_[1] = -1;
  level_ = MonitorLevel::None;
}

std::vector<std::string> Monitor::roots() const {
  std::lock_guard<std::mutex> g(roots_mtx_);
  return opt_.roots;
}

// 热更新监控根（SETROOTS）：调用方需已把新增根的内容写入索引、
// 已把移除根的内容从索引清除；这里同步 watch 集合与 roots 列表。
void Monitor::adopt_roots(const std::vector<std::string>& added,
                          const std::vector<std::string>& removed) {
  {
    std::lock_guard<std::mutex> g(roots_mtx_);
    for (const auto& r : removed)
      opt_.roots.erase(std::remove(opt_.roots.begin(), opt_.roots.end(), r),
                       opt_.roots.end());
    for (const auto& r : added)
      if (std::find(opt_.roots.begin(), opt_.roots.end(), r) ==
          opt_.roots.end())
        opt_.roots.push_back(r);
    roots_snap_ =
        std::make_shared<const std::vector<std::string>>(opt_.roots);
  }
  if (level_ != MonitorLevel::Inotify || inotify_fd_ < 0)
    return;  // fanotify/rescan 模式：由周期对账覆盖新根
  // 撤销移除根下的全部 watch
  if (!removed.empty()) {
    std::lock_guard<std::mutex> g(watch_mtx_);
    for (auto it = wd2dir_.begin(); it != wd2dir_.end();) {
      bool under = false;
      for (const auto& r : removed)
        if (it->second == r || starts_with(it->second, r + "/")) {
          under = true;
          break;
        }
      if (under) {
        ::inotify_rm_watch(inotify_fd_, it->first);
        it = wd2dir_.erase(it);
      } else {
        ++it;
      }
    }
  }
  // 新增根：根自身 + 索引内其全部子目录补 watch（已 watch 的跳过）
  if (!added.empty()) {
    std::unordered_set<std::string> watched;
    {
      std::lock_guard<std::mutex> g(watch_mtx_);
      for (const auto& kv : wd2dir_) watched.insert(kv.second);
    }
    for (const auto& a : added) {
      watch_dir_inotify(a);
      for (const auto& d : idx_.all_dir_paths()) {
        if ((d == a || starts_with(d, a + "/")) && !watched.count(d)) {
          watch_dir_inotify(d);
          watched.insert(d);
        }
      }
    }
  }
}

void Monitor::rescan_now() { rescan_requested_ = true; }

// ---------------- L1: fanotify ----------------

bool Monitor::setup_fanotify(std::string& note) {
  if (opt_.roots.empty()) { note = "无监控根"; return false; }
  fan_fd_ = fanotify_init(FAN_CLOEXEC | FAN_NONBLOCK | FAN_CLASS_NOTIF |
                              FAN_REPORT_DFID_NAME,
                          O_RDONLY | O_LARGEFILE | O_CLOEXEC);
  if (fan_fd_ < 0) { note = errno_note("fanotify_init"); return false; }
  // FAN_ONDIR：无它则 mkdir/rmdir/目录搬入不上报，新目录整树要等对账才可见
  const uint64_t mask = FAN_CREATE | FAN_DELETE | FAN_MOVED_TO | FAN_MOVED_FROM |
                        FAN_DELETE_SELF | FAN_MOVE_SELF | FAN_ONDIR |
                        FAN_CLOSE_WRITE;
  for (const auto& root : opt_.roots) {
    if (fanotify_mark(fan_fd_, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, mask,
                      AT_FDCWD, root.c_str()) != 0) {
      note = errno_note(("fanotify_mark " + root).c_str());
      close(fan_fd_);
      fan_fd_ = -1;
      return false;
    }
  }
  // 路径解析能力探测：name_to_handle_at + open_by_handle_at 需要
  // CAP_DAC_READ_SEARCH（典型场景：守护进程以 root/systemd 系统服务运行）
  mount_fd_ = open(opt_.roots[0].c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (mount_fd_ < 0) {
    note = errno_note("open(root)");
    close(fan_fd_);
    fan_fd_ = -1;
    return false;
  }
  char buf[sizeof(file_handle) + 128];
  file_handle* fh = (file_handle*)buf;
  fh->handle_bytes = 128;
  int mount_id = 0;
  if (name_to_handle_at(AT_FDCWD, opt_.roots[0].c_str(), fh, &mount_id, 0) != 0) {
    note = errno_note("name_to_handle_at");
    close(fan_fd_);
    fan_fd_ = -1;
    close(mount_fd_);
    mount_fd_ = -1;
    return false;
  }
  int probe = open_by_handle_at(mount_fd_, fh, O_RDONLY | O_DIRECTORY);
  if (probe < 0) {
    note = "open_by_handle_at 需要权限（CAP_DAC_READ_SEARCH），fanotify 降级";
    close(fan_fd_);
    fan_fd_ = -1;
    close(mount_fd_);
    mount_fd_ = -1;
    return false;
  }
  close(probe);
  return true;
}

void Monitor::drain_fanotify() {
  char buf[8192] __attribute__((aligned(8)));
  for (;;) {
    ssize_t n = read(fan_fd_, buf, sizeof buf);
    if (n <= 0) break;
    size_t off = 0;
    while (off + sizeof(fanotify_event_metadata) <= (size_t)n) {
      const fanotify_event_metadata* md =
          (const fanotify_event_metadata*)(buf + off);
      if (md->event_len < sizeof(fanotify_event_metadata) ||
          off + md->event_len > (size_t)n)
        break;
      off += md->event_len;
      if (md->fd >= 0) close(md->fd);
      if (md->mask & FAN_Q_OVERFLOW) {
        rescan_requested_ = true;  // 溢出即丢事件：触发对账兜底
        continue;
      }
      bool removed = md->mask & (FAN_DELETE | FAN_MOVED_FROM | FAN_DELETE_SELF);
      // DFID_NAME 记录：目录句柄 + 文件名
      size_t info_off = off - md->event_len + md->metadata_len;
      size_t info_end = off;
      while (info_off + sizeof(fanotify_event_info_header) <= info_end) {
        const fanotify_event_info_header* h =
            (const fanotify_event_info_header*)(buf + info_off);
        // 记录长度合法性：len==0 会死循环，len 越界会读到事件体外
        if (h->len < sizeof(fanotify_event_info_header) ||
            info_off + h->len > info_end)
          break;
        if (h->info_type != FAN_EVENT_INFO_TYPE_DFID_NAME) {
          info_off += h->len;
          continue;
        }
        const fanotify_event_info_fid* fid =
            (const fanotify_event_info_fid*)h;
        // 先验证记录长到足以容纳 file_handle 结构，再读 handle_bytes
        const char* rec_end = buf + info_off + h->len;
        const char* fh_p = (const char*)fid +
            sizeof(fanotify_event_info_header) + sizeof(__kernel_fsid_t);
        if (fh_p + sizeof(file_handle) > rec_end)
          break;  // 截断/畸形记录：丢弃本事件
        const file_handle* fh = (const file_handle*)fh_p;
        // handle_bytes 来自事件载荷：确保 f_handle+bytes+NUL 不越过记录尾
        if (fh->handle_bytes > 128 ||
            (const char*)fh->f_handle + fh->handle_bytes + 1 > rec_end)
          break;
        const char* name =
            (const char*)fh->f_handle + fh->handle_bytes;  // NUL 结尾
        int dirfd = open_by_handle_at(mount_fd_,
                                      const_cast<file_handle*>(fh),
                                      O_RDONLY | O_DIRECTORY);
        if (dirfd >= 0) {
          char link[64], dirpath[4096];
          std::snprintf(link, sizeof link, "/proc/self/fd/%d", dirfd);
          ssize_t len = readlink(link, dirpath, sizeof(dirpath) - 1);
          if (len > 0) {
            dirpath[len] = '\0';
            std::string path = dirpath;
            if (*name) path += '/', path += name;
            bool under_root = false;
            for (const auto& r : opt_.roots)
              if (path == r || starts_with(path, r + "/")) under_root = true;
            if (under_root) {
              std::lock_guard<std::mutex> g(pending_mtx_);
              pending_[path] = removed;
            }
          }
          close(dirfd);
        }
        info_off += h->len;
      }
    }
    if (n < (ssize_t)sizeof buf) break;  // 读尽
  }
}

// ---------------- L2: inotify ----------------

bool Monitor::setup_inotify(std::string& note) {
  inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (inotify_fd_ < 0) { note = errno_note("inotify_init1"); return false; }
  watches_ok_ = watches_fail_ = 0;
  for (const auto& root : opt_.roots) watch_dir_inotify(root);
  for (const auto& dir : idx_.all_dir_paths()) watch_dir_inotify(dir);
  if (watches_ok_ == 0) {
    note = errno_note("inotify_add_watch 全部失败");
    close(inotify_fd_);
    inotify_fd_ = -1;
    return false;
  }
  if (watches_fail_ > 0)
    note = "inotify watch 上限不足（" + std::to_string(watches_fail_) +
           " 个目录未覆盖，靠对账补齐；可调 sysctl fs.inotify.max_user_watches）";
  else
    note = "watch " + std::to_string(watches_ok_) + " 个目录";
  return true;
}

void Monitor::watch_dir_inotify(const std::string& dir) {
  if (inotify_fd_ < 0) return;
  uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                  IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;
  int wd = inotify_add_watch(inotify_fd_, dir.c_str(), mask);
  std::lock_guard<std::mutex> g(watch_mtx_);
  if (wd >= 0) {
    wd2dir_[wd] = dir;
    ++watches_ok_;
  } else {
    ++watches_fail_;
  }
}

void Monitor::add_subtree(const std::string& dir) {
  // 直写索引的旁路必须同步记入回放日志：对账 swap 用扫描开始的快照，
  // 扫描窗口内经此函数补录的条目会被覆盖，flush 路径有 recent_ 兜底而
  // 这里原先没有——新目录整树在 swap 后"消失"的偶发根源。
  auto log_recent = [this](const std::string& p) {
    std::lock_guard<std::mutex> g(recent_mtx_);
    recent_.emplace_back(p, false);
    if (recent_.size() > kRecentCap)
      recent_.erase(recent_.begin(),
                    recent_.begin() + (recent_.size() - kRecentCap));
  };
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (it->is_symlink(e2)) continue;
    std::string p = it->path().string();
    // 与 Scanner 同一套排除表：整树搬入时不把 .git/node_modules 等一并
    // 索引（既污染结果又耗尽 inotify watch 配额）
    if (it->is_directory(e2)) {
      const std::string nm = it->path().filename().string();
      bool excluded_dir = false;
      for (const auto& x : opt_.exclude_names)
        if (nm == x) { excluded_dir = true; break; }
      if (excluded_dir) {
        it.disable_recursion_pending();
        continue;
      }
    }
    if (it->is_directory(e2)) {
      watch_dir_inotify(p);
      std::string nm = it->path().filename().string();
      idx_.upsert(p, 0, 0, EntryType::Dir, pname_of(nm));
      log_recent(p);
    } else if (it->is_regular_file(e2)) {
      std::string nm = it->path().filename().string();
      std::error_code se;
      idx_.upsert(p, it->file_size(se), 0, EntryType::File, pname_of(nm));
      log_recent(p);
    }
  }
}

void Monitor::drain_inotify() {
  char buf[8192] __attribute__((aligned(8)));
  for (;;) {
    ssize_t n = read(inotify_fd_, buf, sizeof buf);
    if (n <= 0) break;
    size_t off = 0;
    while (off + sizeof(inotify_event) <= (size_t)n) {
      const inotify_event* ev = (const inotify_event*)(buf + off);
      off += sizeof(inotify_event) + ev->len;
      if (ev->wd == -1 && (ev->mask & IN_Q_OVERFLOW)) {
        // 内核队列溢出丢过事件：立即触发一次对账兜底（原先是静默丢弃）
        rescan_requested_ = true;
        continue;
      }
      std::string dir;
      {
        std::lock_guard<std::mutex> g(watch_mtx_);
        auto wit = wd2dir_.find(ev->wd);
        if (wit == wd2dir_.end()) continue;
        dir = wit->second;
        if (ev->mask & IN_IGNORED) {  // watch 已被内核移除（目录删除/卸载）
          wd2dir_.erase(wit);
          continue;
        }
        if (ev->len == 0) {  // 目录自身事件（DELETE_SELF / MOVE_SELF）
          if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
            wd2dir_.erase(wit);
            std::lock_guard<std::mutex> g(pending_mtx_);
            pending_[dir] = true;
          }
          continue;
        }
      }
      std::string path = dir + "/" + ev->name;
      if (ev->mask & (IN_CREATE | IN_MOVED_TO)) {
        if (ev->mask & IN_ISDIR) {
          { std::lock_guard<std::mutex> g(pending_mtx_); pending_[path] = false; }
          if (level_ == MonitorLevel::Inotify)
            add_subtree(path);  // 整树搬入时子项无事件，主动补录
        } else {
          std::lock_guard<std::mutex> g(pending_mtx_);
          pending_[path] = false;
        }
      } else if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
        std::lock_guard<std::mutex> g(pending_mtx_);
        pending_[path] = true;
      } else if (ev->mask & IN_CLOSE_WRITE) {
        std::lock_guard<std::mutex> g(pending_mtx_);
        pending_[path] = false;
      }
    }
    if (n < (ssize_t)sizeof buf) break;
  }
}

// ---------------- 事件应用 ----------------

void Monitor::flush_pending() {
  std::map<std::string, bool> batch;
  {
    std::lock_guard<std::mutex> g(pending_mtx_);
    if (pending_.empty()) return;
    batch.swap(pending_);
  }
  // 先记回放日志再应用：若记录在应用之后、恰逢对账线程在两者之间 swap，
  // 该批次会被新索引覆盖又不在回放之列（丢失变更）。先记录则任一时刻
  // swap，批次要么在回放快照里、要么应用在 swap 之后的新索引上。
  {
    std::lock_guard<std::mutex> g(recent_mtx_);
    for (const auto& kv : batch) recent_.emplace_back(kv);
    if (recent_.size() > kRecentCap)
      recent_.erase(recent_.begin(),
                    recent_.begin() + (recent_.size() - kRecentCap));
  }
  for (const auto& kv : batch) apply_one(kv.first, kv.second);
  last_applied_ += batch.size();
  if (opt_.on_applied) opt_.on_applied(batch.size());
  // 节流重建：增量累积超过阈值时重整扁平倒排/rank（100k 约 50ms，风暴场景均摊）
  if (idx_.pending_updates() >= 256) idx_.finalize();
}

// 单条变更入索引（flush 与对账回放共用；按时间序调用保证终态一致）
void Monitor::apply_one(const std::string& path, bool removed) {
  // 热移除根防护：SETROOTS 移除根后在途事件不再回填其路径
  if (!removed) {
    std::shared_ptr<const std::vector<std::string>> snap;
    {
      std::lock_guard<std::mutex> g(roots_mtx_);
      snap = roots_snap_;
    }
    bool under = false;
    for (const auto& r : *snap)
      if (path == r || starts_with(path, r + "/")) {
        under = true;
        break;
      }
    if (!under) return;
  }
  if (opt_.on_change) opt_.on_change(path, removed);
  if (removed) {
    idx_.remove_by_path(path);
    idx_.remove_by_path_prefix(path);  // 目录整体删除时清子孙（文件无害）
    return;
  }
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) {
    // 仅 ENOENT/ENOTDIR 视为删除：EACCES/EIO/NFS 瞬时错误下条目仍在，
    // 误删目录还会经 remove_by_path_prefix 连带清空整个子树
    if (errno == ENOENT || errno == ENOTDIR) {
      idx_.remove_by_path(path);  // 创建后旋即删除
    }
    return;  // 其他错误：保留现状，交由对账修复
  }
  std::string nm = path.substr(path.find_last_of('/') + 1);
  if (S_ISDIR(st.st_mode)) {
    idx_.upsert(path, 0, (std::int64_t)st.st_mtime, EntryType::Dir,
                pname_of(nm));
    if (level_ == MonitorLevel::Inotify) watch_dir_inotify(path);
  } else if (S_ISREG(st.st_mode)) {
    idx_.upsert(path, (u64)st.st_size, (std::int64_t)st.st_mtime,
                EntryType::File, pname_of(nm));
  }
}

// ---------------- 线程体 ----------------

void Monitor::run_event_loop() {
  struct pollfd fds[2];
  fds[0].fd = (level_ == MonitorLevel::Fanotify) ? fan_fd_ : inotify_fd_;
  fds[0].events = POLLIN;
  fds[1].fd = stop_pipe_[0];
  fds[1].events = POLLIN;
  std::int64_t last_event = 0, last_flush = now_ms();
  while (!stop_) {
    fds[0].revents = fds[1].revents = 0;
    int rc = poll(fds, 2, 50);
    if (rc < 0 && errno != EINTR) break;
    if (fds[1].revents & POLLIN) break;
    if (fds[0].revents & POLLIN) {
      if (level_ == MonitorLevel::Fanotify) drain_fanotify();
      else drain_inotify();
      last_event = now_ms();
    }
    std::int64_t debounce = (std::int64_t)opt_.debounce_ms;
    size_t pending_n;
    { std::lock_guard<std::mutex> g(pending_mtx_); pending_n = pending_.size(); }
    // 刷闸：静默去抖（正常路径）之外，事件风暴（去抖窗口永不满足）时
    // 按「距上次落盘 ≥2s 或积压超上限」强制分批应用，防 pending_ 无界增长
    const bool quiet_due = last_event && now_ms() - last_event >= debounce;
    const bool force_due = pending_n >= 4096 || now_ms() - last_flush >= 2000;
    if (pending_n && (quiet_due || force_due)) {
      flush_pending();
      last_flush = now_ms();
    }
  }
  flush_pending();  // 退出前清空
}

void Monitor::run_rescan() {
  auto sleep_until_request_or = [&](int seconds) {
    for (int i = 0; i < seconds * 10 && !stop_ && !rescan_requested_; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    rescan_requested_ = false;
  };
  sleep_until_request_or((int)opt_.rescan_initial_delay_s);
  while (!stop_) {
    FileIndex fresh;
    ScanOptions so;
    so.roots = roots();  // 加锁快照：SETROOTS 热更新可能与对账并发
    so.exclude_names = opt_.exclude_names;
    so.pinyin_fn = opt_.pinyin_fn;
    so.threads = 0;
    Scanner sc(fresh);
    sc.run(so);
    // 对账防自毁：根目录整体不可达（配置指向已删除/未挂载路径）时 fresh
    // 为空，直接 swap 会把热载的完整索引清空（数据"消失"）。空结果仅在
    // 当前索引也为空时才接受，否则保留索引并告警。
    if (fresh.alive_count() == 0 && idx_.alive_count() > 0) {
      if (opt_.on_level)
        opt_.on_level(level_,
                      "对账扫描结果为空（根不可达？），保留现索引不交换");
      if (!stop_) sleep_until_request_or((int)opt_.rescan_interval_s);
      continue;
    }
    idx_.swap(fresh);  // 原子交换：修复漂移 + 压缩墓碑
    // 回放扫描期间应用的实时变更：快照早于这些变更，swap 已将其覆盖，
    // 按原时间序幂等重放到新索引（存在→upsert 最新 stat，不存在→移除）。
    {
      std::deque<std::pair<std::string, bool>> replay;
      {
        std::lock_guard<std::mutex> g(recent_mtx_);
        replay.swap(recent_);
      }
      for (const auto& kv : replay) apply_one(kv.first, kv.second);
      if (!replay.empty()) idx_.finalize();  // 重放量大时重整倒排，保搜索性能
    }
    if (level_ == MonitorLevel::Inotify) {
      // 换索引后 watch 仍然有效（路径不变），补齐可能漏掉的新目录
      for (const auto& d : idx_.all_dir_paths()) watch_dir_inotify(d);
    }
    if (stop_) break;
    sleep_until_request_or((int)opt_.rescan_interval_s);
  }
}

}  // namespace kysearch
