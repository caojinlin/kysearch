// kysearchd —— 索引守护进程（M2）
// 职责：加载/全量扫描索引 → 实时监控（三级降级链）→ Unix socket 查询服务
// 协议（行式，\n 结尾）：
//   PING\n
//   STATUS\n                              -> OK backend=<lv> entries=<n> uptime=<s> roots=<r>
//   SEARCH\t<flags>\t<offset>\t<limit>\t<raw>\n
//     flags 为逗号分隔: p=路径模式 c=区分大小写 w=全字 r=正则
//     -> OK <total>\n  (F|D)\t<size>\t<mtime>\t<path>\n ...  END\n
//   RESCAN\n   -> OK（异步触发对账）
//   STOP\n     -> OK（落盘并退出）
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/index.h"
#include "core/monitor.h"
#include "core/pinyin.h"
#include "core/scanner.h"
#ifdef KYSEARCH_HAVE_SQLITE
#include "core/textindex.h"
#endif
#ifdef KYSEARCH_HAVE_DBUS
#include <dbus/dbus.h>
#endif

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

std::string default_dir() {
  const char* home = std::getenv("HOME");
  std::string d = std::string(home ? home : "/tmp") + "/.cache/kysearch";
  std::error_code ec;
  std::filesystem::create_directories(d, ec);
  return d;
}

std::string default_socket() {
  const char* xdg = std::getenv("XDG_RUNTIME_DIR");
  if (xdg) return std::string(xdg) + "/kysearch.sock";
  const char* home = std::getenv("HOME");
  if (!home) home = "/tmp";
  return std::string(home) + "/.cache/kysearch/daemon.sock";
}

void logline(const char* fmt, ...) {
  std::time_t t = std::time(nullptr);
  char ts[24];
  std::tm tmv{};
  localtime_r(&t, &tmv);
  std::strftime(ts, sizeof ts, "%H:%M:%S", &tmv);
  std::printf("[%s] ", ts);
  va_list ap;
  va_start(ap, fmt);
  std::vprintf(fmt, ap);
  va_end(ap);
  std::printf("\n");
  std::fflush(stdout);
}

bool send_all(int fd, const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
    if (n <= 0) return false;
    off += (size_t)n;
  }
  return true;
}

// 逐行读取（返回 false = 对端关闭/行超限）
constexpr size_t kMaxLine = 64 * 1024;  // 单行上限：防无换行字节流撑爆内存
bool read_line(int fd, std::string& line, std::string& carry) {
  for (;;) {
    size_t nl = carry.find('\n');
    if (nl != std::string::npos) {
      line = carry.substr(0, nl);
      carry.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return true;
    }
    if (carry.size() > kMaxLine) return false;  // 超限断开
    char buf[4096];
    ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n <= 0) return false;
    carry.append(buf, (size_t)n);
  }
}

void handle_client(int fd, kysearch::FileIndex& idx, kysearch::Monitor& mon,
                   const std::vector<std::string>& exclude_names,
                   std::chrono::steady_clock::time_point t0
#ifdef KYSEARCH_HAVE_SQLITE
                   ,
                   kysearch::TextIndex* text
#endif
) {
  std::string carry;
  std::string line;
  while (!g_stop && read_line(fd, line, carry)) {
    if (line == "PING") {
      send_all(fd, "PONG\n");
      continue;
    }
    if (line == "STATUS") {
      auto up = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count();
      std::string roots_j;
      for (const auto& r : mon.roots()) {
        if (!roots_j.empty()) roots_j += ",";
        roots_j += r;
      }
      // std::string 拼接（原固定 1024 栈缓冲会截断多根/长根且丢行尾 \n）
      std::string buf = "OK backend=";
      buf += kysearch::monitor_level_name(mon.level());
      buf += " entries=" + std::to_string(idx.alive_count());
      buf += " uptime=" + std::to_string((long long)up);
      buf += " roots=" + roots_j + "\n";
      send_all(fd, buf);
      continue;
    }
    if (line.rfind("SETROOTS\t", 0) == 0) {
      // 热更新索引根：SETROOTS\t<绝对路径>\t<绝对路径>…
      // （tab 分隔；新增根即时扫描入索引，移除根即时清出索引并撤 watch）
      std::vector<std::string> want;
      {
        const std::string rest = line.substr(9);
        size_t start = 0;
        while (start <= rest.size()) {
          size_t tab = rest.find('\t', start);
          std::string one = rest.substr(
              start, tab == std::string::npos ? std::string::npos : tab - start);
          std::error_code ec;
          auto abs = std::filesystem::absolute(std::filesystem::path(one), ec);
          if (!one.empty() && !ec)
            want.push_back(abs.lexically_normal().string());
          if (tab == std::string::npos) break;
          start = tab + 1;
        }
      }
      if (want.empty()) {
        send_all(fd, "ERR empty roots\n");
        continue;
      }
      const std::vector<std::string> cur = mon.roots();
      std::vector<std::string> added, removed;
      for (const auto& r : want)
        if (std::find(cur.begin(), cur.end(), r) == cur.end())
          added.push_back(r);
      for (const auto& r : cur)
        if (std::find(want.begin(), want.end(), r) == want.end())
          removed.push_back(r);
      for (const auto& r : removed) {
        idx.remove_by_path(r);
        idx.remove_by_path_prefix(r);
      }
      if (!added.empty()) {
        kysearch::ScanOptions so;
        so.roots = added;
        so.exclude_names = exclude_names;
        so.pinyin_fn = kysearch::pinyin_key;
        kysearch::Scanner sc(idx);
        sc.run(so);  // 末尾 finalize 顺带重整倒排/rank
      }
      mon.adopt_roots(added, removed);
      idx.set_roots(mon.roots());  // 路径搜索根语义与监控根保持一致
      std::string rep = "OK applied added=" + std::to_string(added.size()) +
                        " removed=" + std::to_string(removed.size()) +
                        " roots=" + std::to_string(mon.roots().size()) + "\n";
      send_all(fd, rep);
      continue;
    }
    if (line == "RESCAN") {
      mon.rescan_now();
      send_all(fd, "OK\n");
      continue;
    }
    if (line.rfind("SEARCH\t", 0) == 0) {
      // SEARCH\t<flags>\t<offset>\t<limit>\t<raw>  -> 4 段
      std::vector<std::string> parts;
      size_t start = 7;
      for (int i = 0; i < 3; ++i) {  // flags / offset / limit
        size_t tab = line.find('\t', start);
        if (tab == std::string::npos) break;
        parts.push_back(line.substr(start, tab - start));
        start = tab + 1;
      }
      parts.push_back(line.substr(start));  // raw
      if (parts.size() < 4) { send_all(fd, "ERR bad request\n"); continue; }
      kysearch::QueryFlags flags;
      for (char c : parts[0]) {
        if (c == 'p') flags.match_path = true;
        else if (c == 'c') flags.case_sensitive = true;
        else if (c == 'w') flags.whole_word = true;
        else if (c == 'r') flags.regex = true;
      }
      size_t offset = (size_t)std::strtoull(parts[1].c_str(), nullptr, 10);
      size_t limit = (size_t)std::strtoull(parts[2].c_str(), nullptr, 10);
      auto q = kysearch::parse_query(parts[3], flags);
      std::vector<kysearch::u32> res;
      idx.search(q, res);
#ifdef KYSEARCH_HAVE_SQLITE
      if (text && !q.content_terms.empty())
        apply_content_filter(*text, q, idx, res);
#endif
      char head[64];
      std::snprintf(head, sizeof head, "OK %zu\n", res.size());
      send_all(fd, head);
      size_t end = limit ? std::min(res.size(), offset + limit) : res.size();
      for (size_t i = offset; i < end; ++i) {
        // 一次性在共享锁内取回整行（跨锁复用 entry()/opath() 引用会在
        // 并发 upsert 扩容时悬垂）
        kysearch::u64 sz = 0;
        std::int64_t mt = 0;
        kysearch::EntryType ty{};
        std::string path;
        idx.snapshot_row(res[i], sz, mt, ty, path);
        std::string row;
        row.reserve(path.size() + 32);
        row += (ty == kysearch::EntryType::File) ? 'F' : 'D';
        row += '\t';
        row += std::to_string((unsigned long long)sz);
        row += '\t';
        row += std::to_string((long long)mt);
        row += '\t';
        row += path;
        row += '\n';
        if (!send_all(fd, row)) break;
      }
      send_all(fd, "END\n");
      continue;
    }
    if (line == "STOP") {
      send_all(fd, "OK\n");
      g_stop = true;
      continue;
    }
    send_all(fd, "ERR unknown command\n");
  }
  ::close(fd);
}  // （handle_client 结束）

}  // namespace

#ifdef KYSEARCH_HAVE_DBUS
// ---- D-Bus 服务：org.kysearch.Daemon（供 UKUI 搜索面板等生态集成）----
namespace {
kysearch::FileIndex* g_dbus_idx = nullptr;

DBusHandlerResult dbus_handle(DBusConnection* conn, DBusMessage* msg, void*) {
  if (dbus_message_is_method_call(msg, "org.kysearch.Daemon", "Ping")) {
    DBusMessage* reply = dbus_message_new_method_return(msg);
    dbus_connection_send(conn, reply, nullptr);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (dbus_message_is_method_call(msg, "org.kysearch.Daemon", "Search") &&
      g_dbus_idx) {
    const char* flags_s = nullptr;
    const char* query_s = nullptr;
    if (!dbus_message_get_args(msg, nullptr, DBUS_TYPE_STRING, &flags_s,
                               DBUS_TYPE_STRING, &query_s, DBUS_TYPE_INVALID))
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    kysearch::QueryFlags f;
    for (const char* p = flags_s ? flags_s : ""; *p; ++p) {
      if (*p == 'p') f.match_path = true;
      else if (*p == 'c') f.case_sensitive = true;
      else if (*p == 'w') f.whole_word = true;
      else if (*p == 'r') f.regex = true;
    }
    auto q = kysearch::parse_query(query_s ? query_s : "", f);
    std::vector<kysearch::u32> res;
    g_dbus_idx->search(q, res);
    DBusMessage* reply = dbus_message_new_method_return(msg);
    DBusMessageIter it, arr;
    dbus_message_iter_init_append(reply, &it);
    dbus_uint32_t total = (dbus_uint32_t)res.size();
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &total);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
    size_t limit = res.size() < 1000 ? res.size() : 1000;
    for (size_t i = 0; i < limit; ++i) {
      const char* p = nullptr;
      std::string path(g_dbus_idx->opath(res[i]));
      p = path.c_str();
      dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &p);
    }
    dbus_message_iter_close_container(&it, &arr);
    dbus_connection_send(conn, reply, nullptr);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void dbus_thread_fn(std::atomic<bool>& stop) {
  DBusError err;
  dbus_error_init(&err);
  DBusConnection* conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
  if (!conn) {
    logline("D-Bus 不可用：%s（无会话总线时属预期）", err.message);
    dbus_error_free(&err);
    return;
  }
  if (dbus_bus_request_name(conn, "org.kysearch.Daemon",
                            DBUS_NAME_FLAG_REPLACE_EXISTING, &err) !=
      DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
    logline("D-Bus 注册名称失败：%s", err.message ? err.message : "?");
    dbus_error_free(&err);
    return;
  }
  static DBusObjectPathVTable vtable{};
  vtable.message_function = dbus_handle;
  dbus_connection_register_object_path(conn, "/org/kysearch/Daemon", &vtable,
                                       nullptr);
  logline("D-Bus 服务就绪: org.kysearch.Daemon");
  while (!stop)
    dbus_connection_read_write_dispatch(conn, 200);
  dbus_connection_unref(conn);
}
}  // namespace
#endif

int main(int argc, char** argv) {
  std::vector<std::string> roots;
  std::string db = default_dir() + "/index.bin";
  std::string sock = default_socket();
  std::string level = "auto";
  unsigned rescan_interval = 600;
  unsigned rescan_initial = 15;
  bool quiet = false;
  std::string content_roots;  // 逗号分隔；空 = 关闭全文索引
  std::vector<std::string> exclude_names;
  std::string config_path;
  bool use_dbus = false;

  // 配置文件（key=value；命令行参数覆盖同名配置）
  auto load_config = [&](const std::string& path) {
    std::ifstream cf(path);
    if (!cf) return false;
    std::string line;
    auto split_csv = [](const std::string& v, std::vector<std::string>& out) {
      std::stringstream ss(v);
      std::string s;
      while (std::getline(ss, s, ','))
        if (!s.empty()) out.push_back(s);
    };
    while (std::getline(cf, line)) {
      size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string k = line.substr(0, eq), v = line.substr(eq + 1);
      if (k == "roots") { roots.clear(); split_csv(v, roots); }
      else if (k == "exclude") { split_csv(v, exclude_names); }
      else if (k == "content-roots") content_roots = v;
      else if (k == "level") level = v;
      else if (k == "rescan-interval") rescan_interval = (unsigned)std::atoi(v.c_str());
      else if (k == "rescan-initial-delay") rescan_initial = (unsigned)std::atoi(v.c_str());
    }
    return true;
  };
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      config_path = argv[++i];
      break;
    }
  if (!config_path.empty() && !load_config(config_path))
    logline("配置文件读取失败: %s（使用默认值）", config_path.c_str());
  // 未显式指定 --config 时自动加载 GUI 设置页生成的默认配置
  // （systemd 用户服务 ExecStart 不带参数，依赖此默认路径）
  if (config_path.empty()) {
    const char* home = std::getenv("HOME");
    if (home) {
      std::string def =
          std::string(home) + "/.config/kysearch/kysearchd.conf";
      std::error_code ec;
      if (std::filesystem::exists(def, ec)) load_config(def);
    }
  }
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--config") { ++i; continue; }
    else if (a == "--root") roots.push_back(next());
    else if (a == "--db") db = next();
    else if (a == "--socket") sock = next();
    else if (a == "--level") level = next();
    else if (a == "--content-roots") content_roots = next();
    else if (a == "--rescan-interval") rescan_interval = (unsigned)std::atoi(next().c_str());
    else if (a == "--rescan-initial-delay") rescan_initial = (unsigned)std::atoi(next().c_str());
    else if (a == "--exclude") {
      std::stringstream ss(next());
      std::string s;
      while (std::getline(ss, s, ','))
        if (!s.empty()) exclude_names.push_back(s);
    }
    else if (a == "--dbus") use_dbus = true;
    else if (a == "--quiet") quiet = true;
    else if (a == "--config") config_path = next();
    else if (a == "--exclude") { std::string v = next(); std::stringstream ss(v); std::string s; while (std::getline(ss, s, ',')) if (!s.empty()) exclude_names.push_back(s); }
    else if (a == "--help" || a == "-h") {
      std::printf("kysearchd [--root PATH]... [--db P] [--socket P] [--level auto|fanotify|inotify|rescan]\n"
                  "           [--rescan-interval S] [--rescan-initial-delay S] [--quiet]\n");
      return 0;
    }
  }
  if (roots.empty()) {
    const char* home = std::getenv("HOME");
    roots.push_back(home ? home : "/tmp");
  }
  // 根目录规整为绝对路径
  for (auto& r : roots) {
    std::error_code ec;
    auto abs = std::filesystem::absolute(std::filesystem::path(r), ec);
    if (!ec) r = abs.lexically_normal().string();
  }

  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);
  signal(SIGPIPE, SIG_IGN);

  // socket 探活：必须在加载索引/启动 monitor/saver 之前完成——发现活实例
  // 立即退出，绝不触碰对方 socket，也不会留下 joinable 线程
  {
    auto probe_live = [&]() -> bool {
      if (sock.size() >= sizeof(sockaddr_un::sun_path)) {
        logline("socket 路径过长: %s", sock.c_str());
        return true;
      }
      int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
      sockaddr_un pa{};
      pa.sun_family = AF_UNIX;
      std::strncpy(pa.sun_path, sock.c_str(), sizeof pa.sun_path - 1);
      if (probe < 0) return false;
      if (::connect(probe, (sockaddr*)&pa, sizeof pa) == 0) {
        ::close(probe);
        return true;
      }
      ::close(probe);
      struct stat st{};
      if (::lstat(sock.c_str(), &st) == 0) {
        if (S_ISSOCK(st.st_mode))
          ::unlink(sock.c_str());  // 陈旧 socket 文件（ECONNREFUSED）
        else {
          logline("socket 路径被非 socket 文件占用: %s，拒绝覆盖",
                  sock.c_str());
          return true;
        }
      }
      return false;
    };
    if (probe_live()) {
      logline("已有实例在监听 %s，退出（避免抢走 socket）", sock.c_str());
      return 1;
    }
  }

  kysearch::FileIndex idx;
  auto t_start = std::chrono::steady_clock::now();
  if (std::filesystem::exists(db) && kysearch::FileIndex::load(db, idx)) {
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t_start)
                    .count();
    if (!quiet) logline("快照热载 %zu 项 (%.1f ms)", idx.alive_count(), ms);
  } else {
    if (!quiet) logline("无快照，开始全量扫描 %s …", roots[0].c_str());
    kysearch::ScanOptions so;
    so.roots = roots;
    so.exclude_names = exclude_names;
    so.pinyin_fn = kysearch::pinyin_key;  // 中文拼音索引
    kysearch::Scanner sc(idx);
    sc.run(so);
    idx.save(db);
    if (!quiet)
      logline("全量扫描完成 %zu 项，用时 %lld ms", idx.alive_count(),
              (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t_start)
                              .count());
  }

  kysearch::Monitor::Options mo;
  mo.roots = roots;
  mo.preferred = level;
  mo.rescan_interval_s = rescan_interval;
  mo.rescan_initial_delay_s = rescan_initial;
  mo.pinyin_fn = kysearch::pinyin_key;  // 增量/对账路径同样生成拼音键
  mo.exclude_names = exclude_names;

  // ---- 全文内容索引（可选）----
#ifdef KYSEARCH_HAVE_SQLITE
  kysearch::TextIndex text_;
  std::deque<std::pair<std::string, bool>> content_q;  // path, removed
  std::mutex content_mtx;
  std::condition_variable content_cv;
  std::thread content_thread;
  if (!content_roots.empty()) {
    std::string cdir = db.substr(0, db.find_last_of('/'));
    std::error_code ec;
    std::filesystem::create_directories(cdir, ec);
    if (text_.open(cdir + "/content.db")) {
      mo.on_change = [&](const std::string& p, bool removed) {
        if (kysearch::TextIndex::want_file(p)) {
          {
            std::lock_guard<std::mutex> g(content_mtx);
            content_q.emplace_back(p, removed);
          }
          content_cv.notify_one();
        }
      };
      std::vector<std::string> croots;
      std::stringstream cr(content_roots);
      std::string seg;
      while (std::getline(cr, seg, ','))
        if (!seg.empty()) croots.push_back(seg);
      // mtime+size 未变则跳过重读（CLOSE_WRITE 风暴抑制）。
      // 共享指针：线程生命周期可能越过局部 seen 的作用域（按引用捕获
      // 会在 main 收尾时悬垂）
      auto seen = std::make_shared<
          std::unordered_map<std::string,
                             std::pair<std::int64_t, kysearch::u64>>>();
      content_thread = std::thread([&, seen, croots] {
        namespace cfs = std::filesystem;
        auto index_if_needed = [&](const std::string& p) {
          struct stat st {};
          if (::stat(p.c_str(), &st) != 0) return;
          auto it = seen->find(p);
          if (it != seen->end() && it->second.first == (std::int64_t)st.st_mtime &&
              it->second.second == (kysearch::u64)st.st_size)
            return;  // 内容未变
          if (text_.index_file(p))
            (*seen)[p] = {(std::int64_t)st.st_mtime, (kysearch::u64)st.st_size};
        };
        // 初始全量
        for (const auto& root : croots) {
          std::error_code e1;
          for (cfs::recursive_directory_iterator
                   it(root, cfs::directory_options::skip_permission_denied, e1),
               end;
               !e1 && it != end; it.increment(e1)) {
            if (g_stop) return;
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            std::string p = it->path().string();
            std::error_code se;
            kysearch::u64 sz = it->file_size(se);
            if (!se && kysearch::TextIndex::want_file(
                           p, kysearch::TextIndex::kDefaultMaxBytes, sz))
              index_if_needed(p);
          }
        }
        if (!quiet) logline("全文内容索引就绪：%zu 篇", text_.count());
        // 增量（事件驱动）
        while (!g_stop) {
          std::pair<std::string, bool> item;
          {
            std::unique_lock<std::mutex> lk(content_mtx);
            content_cv.wait_for(lk, std::chrono::milliseconds(200), [&] {
              return !content_q.empty() || g_stop;
            });
            if (content_q.empty()) continue;
            item = content_q.front();
            content_q.pop_front();
          }
          if (item.second) {
            text_.remove_file(item.first);
            seen->erase(item.first);
          } else {
            index_if_needed(item.first);
          }
        }
      });
    } else if (!quiet) {
      logline("全文索引不可用（SQLite FTS5 缺失），content: 查询将被忽略");
    }
  }
#endif
  mo.on_level = [&](kysearch::MonitorLevel lv, const std::string& note) {
    if (!quiet)
      logline("监控级别: %s（%s）", kysearch::monitor_level_name(lv), note.c_str());
  };
  mo.on_applied = [&](size_t n) {
    if (!quiet) logline("实时事件应用 %zu 条", n);
  };
  kysearch::Monitor mon(idx, mo);
  mon.start();

  // 周期落盘线程
  std::thread saver([&] {
    while (!g_stop) {
      for (int i = 0; i < 300 && !g_stop; ++i)
        std::this_thread::sleep_for(std::chrono::seconds(1));
      if (g_stop) break;
      idx.save(db);
      if (!quiet) logline("周期快照已保存");
    }
  });

  // Unix socket 服务（先探测存活实例与陈旧 socket，拒绝误删普通文件）
  bool sock_owned = false;  // 本进程是否成功 bind（决定退出时可否 unlink）
  int lfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, sock.c_str(), sizeof addr.sun_path - 1);
  if (::bind(lfd, (sockaddr*)&addr, sizeof addr) != 0 || ::listen(lfd, 16) != 0) {
    logline("socket 建立失败: %s (%s)", sock.c_str(), std::strerror(errno));
    g_stop = true;
  } else {
    sock_owned = true;
    // 仅属主可连（配合 accept 后 SO_PEERCRED 校验；如需开放其他用户，
    // 放宽 chmod 并保持 PEERCRED 白名单）
    ::chmod(sock.c_str(), 0600);
    if (!quiet) {
      logline("kysearchd 就绪: socket=%s level=%s entries=%zu", sock.c_str(),
              kysearch::monitor_level_name(mon.level()), idx.alive_count());
    }
  }

#ifdef KYSEARCH_HAVE_DBUS
  std::thread dbus_thread;
  if (use_dbus) {
    g_dbus_idx = &idx;
    dbus_thread = std::thread(dbus_thread_fn, std::ref(g_stop));
  }
#endif
  // 客户端线程登记：退出时可唤醒并 join（原 detach 版在 main 销毁局部
  // 对象时可能 use-after-free）；上限防本机连接风暴
  struct Client {
    std::thread th;
    std::shared_ptr<std::atomic<bool>> done;
    int fd;
  };
  std::mutex clients_mtx;
  std::vector<Client> clients;
  auto reap_clients = [&] {  // 回收已结束线程（防止长期运行句柄堆积）
    std::lock_guard<std::mutex> g(clients_mtx);
    for (auto it = clients.begin(); it != clients.end();) {
      if (it->done->load()) {
        it->th.join();
        it = clients.erase(it);
      } else {
        ++it;
      }
    }
  };
  auto client_main = [&](int cfd, std::shared_ptr<std::atomic<bool>> done) {
    handle_client(cfd, idx, mon, exclude_names, t_start
#ifdef KYSEARCH_HAVE_SQLITE
                  ,
                  &text_
#endif
    );
    done->store(true);
  };

  int poll_ticks = 0;
  while (!g_stop) {
    struct pollfd pfd{ lfd, POLLIN, 0 };
    int rc = ::poll(&pfd, 1, 500);
    if (++poll_ticks % 20 == 0) reap_clients();  // 约每 10s 回收
    if (rc <= 0) continue;
    if (!(pfd.revents & POLLIN)) continue;
    int cfd = ::accept(lfd, nullptr, nullptr);
    if (cfd < 0) continue;
    // 对端凭证校验：仅同 uid 或 root（配合 socket 0600 双保险）
    struct ucred cred{};
    socklen_t clen = sizeof cred;
    if (::getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) != 0 ||
        (cred.uid != geteuid() && cred.uid != 0)) {
      ::close(cfd);
      continue;
    }
    {
      std::lock_guard<std::mutex> g(clients_mtx);
      if (clients.size() >= 64) {  // 并发上限
        ::close(cfd);
        continue;
      }
    }
    // 30s 接收超时：空闲客户端不会永久滞留
    struct timeval tv{ 30, 0 };
    ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread th(client_main, cfd, done);
    std::lock_guard<std::mutex> g(clients_mtx);
    clients.push_back(Client{std::move(th), std::move(done), cfd});
  }

  if (!quiet) logline("正在退出：停止监控并保存快照…");
  mon.stop();  // 先停监控（flush 末批事件），再统一落盘
#ifdef KYSEARCH_HAVE_DBUS
  if (dbus_thread.joinable()) dbus_thread.join();
#endif
#ifdef KYSEARCH_HAVE_SQLITE
  if (content_thread.joinable()) content_thread.join();
#endif
  saver.join();  // saver 退出前可能正在 save——必须 join 后再最终落盘（并发双写同名 tmp 会损坏快照）
  idx.save(db);  // 最终快照由 main 独占写出
  {  // 唤醒并回收全部客户端线程（recv 阻塞中的线程由 shutdown 解除）
    std::lock_guard<std::mutex> g(clients_mtx);
    for (auto& c : clients) ::shutdown(c.fd, SHUT_RDWR);
    for (auto& c : clients)
      if (c.th.joinable()) c.th.join();
  }
  ::close(lfd);
  if (sock_owned) ::unlink(sock.c_str());  // 仅清理自己 bind 的 socket
  return 0;
}
