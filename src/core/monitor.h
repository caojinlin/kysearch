// kysearch 实时监控（M2）：三级降级链
//   L1 fanotify：文件系统级标记（内核>=5.1），路径解析需要 CAP_DAC_READ_SEARCH；
//      探测失败自动降级 —— 普通用户守护进程通常落在 L2
//   L2 inotify：对索引内全部目录建 watch；watch 上限耗尽（ENOSPC）时保留
//      局部覆盖，由 L3 对账补齐；完全失败降级 L3
//   L3 定时对账重扫：无条件常驻（默认 600s），全量重扫后原子交换索引，
//      同时修复任何事件丢失导致的索引漂移、并压缩墓碑
#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/index.h"

namespace kysearch {

enum class MonitorLevel { None = 0, Fanotify, Inotify, Rescan };
const char* monitor_level_name(MonitorLevel lv);

class Monitor {
 public:
  struct Options {
    std::vector<std::string> roots;
    std::string preferred = "auto";  // auto | fanotify | inotify | rescan
    unsigned debounce_ms = 200;
    unsigned rescan_interval_s = 600;
    unsigned rescan_initial_delay_s = 15;  // 快照热载后的首次对账
    std::vector<std::string> exclude_names;  // 对账重扫时沿用扫描排除表
    std::function<void(MonitorLevel, const std::string& note)> on_level;
    std::function<void(size_t applied)> on_applied;  // 每批事件应用后
    // 每条变更回调（内容索引等旁路消费者使用；目录删除仅回调目录本身）
    std::function<void(const std::string& path, bool removed)> on_change;
    std::function<std::string(const std::string& name)> pinyin_fn;
  };

  Monitor(FileIndex& idx, Options opt);
  ~Monitor();
  Monitor(const Monitor&) = delete;
  Monitor& operator=(const Monitor&) = delete;

  void start();
  void stop();
  MonitorLevel level() const { return level_.load(); }
  void rescan_now();  // 异步请求一次对账重扫
  size_t last_applied() const { return last_applied_.load(); }

 private:
  void run_event_loop();
  void run_rescan();
  bool setup_fanotify(std::string& note);
  bool setup_inotify(std::string& note);
  void drain_fanotify();
  void drain_inotify();
  void flush_pending();
  void apply_one(const std::string& path, bool removed);  // 单条变更入索引
  void watch_dir_inotify(const std::string& dir);
  void add_subtree(const std::string& dir);  // 新目录整树补录（含自身）
  std::string pname_of(const std::string& name) {
    return opt_.pinyin_fn ? opt_.pinyin_fn(name) : std::string();
  }

  FileIndex& idx_;
  Options opt_;
  std::atomic<MonitorLevel> level_{MonitorLevel::None};
  std::atomic<bool> stop_{false};
  std::atomic<bool> rescan_requested_{false};
  std::atomic<size_t> last_applied_{0};
  std::thread event_thread_, rescan_thread_;
  int fan_fd_ = -1;
  int inotify_fd_ = -1;
  int mount_fd_ = -1;
  int stop_pipe_[2] = {-1, -1};
  std::unordered_map<int, std::string> wd2dir_;
  std::mutex pending_mtx_;
  std::map<std::string, bool> pending_;  // path -> removed（后到事件覆盖先到）
  // 已应用变更的回放日志：对账扫描期间的实时 upsert 会被 swap 覆盖
  // （快照早于变更），swap 后按序幂等重放，堵住「扫描窗口丢变更」竞态。
  // 有界环形，超限丢最旧（极端风暴下次对账兜底修复漂移）。
  static constexpr size_t kRecentCap = 65536;
  std::mutex recent_mtx_;
  std::deque<std::pair<std::string, bool>> recent_;
  // wd2dir_ 会被事件线程（apply_one/add_subtree）与对账线程（补 watch/回放）
  // 并发读写；stop() 亦可能被并发调用（重复 join 属 UB）——各自加锁
  std::mutex watch_mtx_;
  std::mutex stop_mtx_;
  size_t watches_ok_ = 0, watches_fail_ = 0;
};

}  // namespace kysearch
