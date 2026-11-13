#include "core/scanner.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace kysearch {

namespace {

struct RawRecord {
  std::string opath, lname, pname;
  u64 size;
  std::int64_t mtime;
  EntryType type;
};

inline char ascii_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

std::string lower(std::string s) {
  for (auto& c : s) c = ascii_lower(c);
  return s;
}

// libstdc++ 的 file_clock 纪元不是 unix epoch（差 6437454000 秒，表现为年份回到 1822）。
// 启动时用两个时钟的 now() 差值标定偏移，可移植且不依赖实现细节。
std::int64_t mtime_of(const fs::directory_entry& de) {
  std::error_code ec;
  auto t = de.last_write_time(ec);
  if (ec) return 0;
  using namespace std::chrono;
  static const std::int64_t epoch_offset = [] {
    auto f = fs::file_time_type::clock::now().time_since_epoch();
    auto s = std::chrono::system_clock::now().time_since_epoch();
    using namespace std::chrono;
    return (duration_cast<nanoseconds>(s) - duration_cast<nanoseconds>(f))
               .count() /
           1000000000;
  }();
  return duration_cast<seconds>(t.time_since_epoch()).count() + epoch_offset;
}

}  // namespace

void Scanner::run(const ScanOptions& opt) {
  const unsigned hw = std::thread::hardware_concurrency();
  const unsigned nthr =
      opt.threads ? opt.threads : (hw ? std::min(hw, 16u) : 1u);

  std::mutex qmtx, imtx;
  std::condition_variable cv;
  std::deque<std::string> queue;
  std::atomic<size_t> pending{0};
  std::atomic<size_t> nfiles{0}, ndirs{0};
  std::atomic<size_t> last_report{0};  // 多 worker 并发读改写（原裸 size_t 为数据竞争）

  // 根目录统一成绝对路径（展示与路径搜索才稳定）
  std::vector<std::string> roots;
  for (const auto& r : opt.roots) {
    std::error_code ec;
    fs::path abs = fs::absolute(fs::path(r), ec);
    if (ec) continue;
    roots.push_back(abs.lexically_normal().string());
  }
  if (roots.empty()) {
    // 空根也必须 finalize：search() 在未 finalize 时返回空，
    // 调用方依赖 run() 收尾（否则索引永久不可查）
    idx_.finalize();
    return;
  }
  idx_.set_roots(roots);
  for (const auto& r : roots) {
    queue.push_back(r);
    pending++;
  }

  auto excluded = [&opt](const std::string& name) {
    for (const auto& x : opt.exclude_names)
      if (name == x) return true;
    return false;
  };

  auto flush = [&](std::vector<RawRecord>& batch) {
    if (batch.empty()) return;
    {
      std::lock_guard<std::mutex> lk(imtx);
      for (const auto& r : batch)
        idx_.add(r.lname, r.opath, r.pname, r.size, r.mtime, r.type);
    }
    batch.clear();
    size_t seen = nfiles.load() + ndirs.load();
    // CAS 抢占上报权：避免多 worker 同时跨阈值重复回调
    size_t lr = last_report.load();
    if (opt.progress && seen - lr >= 4096 &&
        last_report.compare_exchange_strong(lr, seen))
      opt.progress(nfiles.load(), ndirs.load());
  };

  auto worker = [&] {
    std::vector<RawRecord> batch;
    batch.reserve(512);
    auto push = [&](const fs::path& p, u64 size, std::int64_t mt, EntryType t) {
      std::string op = p.string();
      std::string ln = op.substr(op.find_last_of('/') + 1);
      std::string pn;
      if (opt.pinyin_fn) pn = opt.pinyin_fn(ln);  // 拼音键基于原始文件名
      ln = lower(std::move(ln));                  // lname 统一小写
      batch.push_back(
          RawRecord{std::move(op), std::move(ln), std::move(pn), size, mt, t});
    };
    for (;;) {
      std::string dir;
      {
        std::unique_lock<std::mutex> lk(qmtx);
        cv.wait(lk, [&] { return !queue.empty() || pending == 0; });
        if (queue.empty() && pending == 0) break;
        if (opt.cancel && opt.cancel->load()) break;  // 取消：弃置剩余队列
        dir = std::move(queue.front());
        queue.pop_front();
      }
      std::error_code it_ec;
      fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied,
                                it_ec);
      for (fs::directory_iterator end; !it_ec && it != end; it.increment(it_ec)) {
        std::error_code e2;
        if (it->is_symlink(e2)) continue;  // 跳过符号链接：防环、防重复
        if (it->is_directory(e2)) {
          ndirs++;
          push(it->path(), 0, mtime_of(*it), EntryType::Dir);
          if (!excluded(it->path().filename().string())) {
            // 入队与 pending++ 同锁：锁外自增存在被并发 pending-- 抢先
            // 减到 0/下溢的窗口（worker 提前退出乃至永久等待）
            {
              std::lock_guard<std::mutex> lk(qmtx);
              queue.push_back(it->path().string());
              pending++;
            }
            cv.notify_one();
          }
        } else if (it->is_regular_file(e2)) {
          nfiles++;
          std::error_code se;
          u64 sz = it->file_size(se);
          if (se) sz = 0;
          push(it->path(), sz, mtime_of(*it), EntryType::File);
        }
        if (batch.size() >= 512) flush(batch);
        if (opt.cancel && opt.cancel->load()) break;  // 取消：尽快收尾
      }
      flush(batch);
      {  // 出队计数与判定同锁（notify 可在锁外）
        std::lock_guard<std::mutex> lk(qmtx);
        pending--;
      }
      cv.notify_all();  // 让等待的线程重新判定退出条件
    }
  };

  std::vector<std::thread> pool;
  for (unsigned i = 0; i < nthr; ++i) pool.emplace_back(worker);
  for (auto& t : pool) t.join();

  if (opt.progress) opt.progress(nfiles.load(), ndirs.load());
  idx_.finalize();
}

}  // namespace kysearch
