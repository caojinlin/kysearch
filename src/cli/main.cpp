// kysearch-cli —— 命令行入口（无头测试/调试/基准用）
// 用法：
//   kysearch-cli scan <root>... [--db PATH] [--threads N] [--progress]
//   kysearch-cli search [--db PATH] [--path] [--case] [--word] [--limit N]
//                       [--bench K] <terms...>
//   kysearch-cli info [--db PATH]
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <string>
#include <vector>

#include "core/fsutil.h"
#include "core/index.h"
#include "core/pinyin.h"
#include "core/scanner.h"
#ifdef KYSEARCH_HAVE_SQLITE
#include "core/textindex.h"
#endif

namespace {

// ---- Unix socket 客户端（连 kysearchd）----

bool send_cmd(const std::string& sock, const std::string& cmd, std::string& out) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, sock.c_str(), sizeof addr.sun_path - 1);
  if (::connect(fd, (sockaddr*)&addr, sizeof addr) != 0) {
    ::close(fd);
    return false;
  }
  std::string req = cmd + "\n";
  // 流式 send 需循环至发完（部分写并非失败）
  for (size_t off = 0; off < req.size();) {
    ssize_t n = ::send(fd, req.data() + off, req.size() - off, MSG_NOSIGNAL);
    if (n <= 0) { ::close(fd); return false; }
    off += (size_t)n;
  }
  ::shutdown(fd, SHUT_WR);
  char buf[8192];
  for (;;) {
    ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n <= 0) break;
    out.append(buf, (size_t)n);
  }
  ::close(fd);
  return true;
}

std::string default_dir() {
  const char* home = std::getenv("HOME");
  if (!home) home = "/tmp";
  std::string dir = std::string(home) + "/.cache/kysearch";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

std::string default_db() { return default_dir() + "/index.bin"; }


std::string default_socket() {
  const char* xdg = std::getenv("XDG_RUNTIME_DIR");
  if (xdg) return std::string(xdg) + "/kysearch.sock";
  const char* home = std::getenv("HOME");
  if (!home) home = "/tmp";
  return std::string(home) + "/.cache/kysearch/daemon.sock";
}


std::string human_size(kysearch::u64 b) {
  char buf[32];
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  int u = 0;
  double v = (double)b;
  while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
  if (u == 0) std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b);
  else std::snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
  return buf;
}

std::string fmt_time(std::int64_t sec) {
  char buf[24];
  std::time_t t = (std::time_t)sec;
  std::tm tmv{};
  localtime_r(&t, &tmv);
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tmv);
  return buf;
}

unsigned long long parse_ull(const std::string& v, bool& ok) {
  ok = false;
  if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos)
    return 0;
  try {
    size_t pos = 0;
    unsigned long long r = std::stoull(v, &pos);
    if (pos == v.size()) { ok = true; return r; }
  } catch (...) {
  }
  return 0;
}

void print_entry(const kysearch::FileIndex& idx, kysearch::u32 i) {
  const auto& e = idx.entry(i);
  std::cout << (e.type == kysearch::EntryType::File ? "F" : "D") << '\t'
            << human_size(e.size) << '\t' << fmt_time(e.mtime_sec) << '\t'
            << idx.opath(i) << '\n';
}

int cmd_scan(std::vector<std::string>& args) {
  std::string db = default_db();
  unsigned threads = 0;
  bool progress = false;
  std::vector<std::string> roots;
  auto next_val = [&args](size_t& i) -> std::string {
    // 带值参数统一取值：越界/缺值给空串（原 ++i 越界为 UB）
    return i + 1 < args.size() ? args[++i] : std::string();
  };
  for (size_t i = 0; i < args.size(); ++i) {
    const auto& a = args[i];
    if (a == "--db") db = next_val(i);
    else if (a == "--threads") {
      bool pok = false;
      unsigned long long v = parse_ull(next_val(i), pok);
      if (pok) threads = (unsigned)v;
    }
    else if (a == "--progress") progress = true;
    else roots.push_back(a);
  }
  if (roots.empty()) { std::cerr << "用法: scan <root>...\n"; return 2; }

  kysearch::FileIndex idx;
  kysearch::ScanOptions opt;
  opt.roots = roots;
  opt.threads = threads;
  opt.pinyin_fn = kysearch::pinyin_key;  // 中文拼音索引
  if (progress)
    opt.progress = [](size_t f, size_t d) {
      std::fprintf(stderr, "\r索引中 files=%zu dirs=%zu    ", f, d);
      std::fflush(stderr);
    };

  auto t0 = std::chrono::steady_clock::now();
  kysearch::Scanner sc(idx);
  sc.run(opt);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();

  if (progress) std::fprintf(stderr, "\n");
  auto st = idx.stats();
  std::printf("已索引 files + dirs = %zu 项，用时 %lld ms，内存池 %.1f MB\n",
              st.entry_count, (long long)ms,
              (st.lname_bytes + st.opath_bytes + st.pname_bytes +
               st.entry_count * sizeof(kysearch::Entry)) / 1048576.0);
  if (idx.save(db)) std::printf("快照已保存: %s (%.1f MB)\n", db.c_str(),
                                std::filesystem::file_size(db) / 1048576.0);
  else { std::fprintf(stderr, "快照保存失败: %s\n", db.c_str()); return 1; }
  return 0;
}

int cmd_search(std::vector<std::string>& args) {
  std::string db = default_db();
  std::string socket, csv_path;
  kysearch::QueryFlags flags;
  size_t limit = 0, bench = 0;
  std::string raw;
  for (size_t i = 0; i < args.size(); ++i) {
    const auto& a = args[i];
    if (a == "--db") { if (i + 1 < args.size()) db = args[++i]; }
    else if (a == "--socket") { if (i + 1 < args.size()) socket = args[++i]; }
    else if (a == "--path") flags.match_path = true;
    else if (a == "--case") flags.case_sensitive = true;
    else if (a == "--word") flags.whole_word = true;
    else if (a == "--regex") flags.regex = true;
    else if (a == "--limit") { if (i + 1 < args.size()) { bool pok = false; unsigned long long v = parse_ull(args[i + 1], pok); if (pok) { limit = (size_t)v; ++i; } } }
    else if (a == "--bench") { if (i + 1 < args.size()) { bool pok = false; unsigned long long v = parse_ull(args[i + 1], pok); if (pok) { bench = (size_t)v; ++i; } } }
    else if (a == "--csv") { if (i + 1 < args.size()) csv_path = args[++i]; }
    else { if (!raw.empty()) raw += ' '; raw += a; }
  }

  // 远程模式：查询守护进程（享受实时索引）
  if (!socket.empty()) {
    std::string f;
    if (flags.match_path) f += 'p';
    if (flags.case_sensitive) f += 'c';
    if (flags.whole_word) f += 'w';
    if (flags.regex) f += 'r';
    std::string cmd = "SEARCH\t" + f + "\t0\t" + std::to_string(limit) + "\t" + raw;
    std::string out;
    if (!send_cmd(socket, cmd, out)) {
      std::fprintf(stderr, "无法连接守护进程 %s\n", socket.c_str());
      return 1;
    }
    size_t shown = 0;
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
      if (line.empty() || line == "END") continue;
      if (line.rfind("OK ", 0) == 0) { std::cout << "# 命中 " << line.substr(3) << " 项（守护进程实时索引）\n"; continue; }
      if (line.rfind("ERR", 0) == 0) { std::cerr << line << "\n"; return 1; }
      if (limit && shown >= limit) continue;
      // F/D<TAB>size<TAB>mtime<TAB>path -> 原样人类可读输出
      auto t1 = line.find('\t'); auto t2 = line.find('\t', t1 + 1); auto t3 = line.find('\t', t2 + 1);
      if (t3 == std::string::npos) { std::cout << line << "\n"; continue; }
      std::string type = line.substr(0, t1), sz = line.substr(t1 + 1, t2 - t1 - 1),
                  mt = line.substr(t2 + 1, t3 - t2 - 1), p = line.substr(t3 + 1);
      std::cout << (type == "F" ? "F" : "D") << '\t' << sz << '\t'
                << fmt_time((std::int64_t)std::atoll(mt.c_str())) << '\t' << p << '\n';
      ++shown;
    }
    return 0;
  }

  kysearch::FileIndex idx;
  if (!kysearch::FileIndex::load(db, idx)) {
    std::fprintf(stderr, "无法加载索引 %s，请先执行 scan\n", db.c_str());
    return 1;
  }
  auto q = kysearch::parse_query(raw, flags);

  std::vector<kysearch::u32> res;
  idx.search(q, res);
#ifdef KYSEARCH_HAVE_SQLITE
  if (!q.content_terms.empty()) {
    size_t slash = db.find_last_of('/');
    std::string cdb = (slash == std::string::npos ? "." : db.substr(0, slash)) +
                      "/content.db";
    kysearch::TextIndex t;
    if (!t.open(cdb)) {
      std::fprintf(stderr, "内容索引不可用（%s）\n", cdb.c_str());
      return 1;
    }
    kysearch::apply_content_filter(t, q, idx, res);
  }
#endif
  if (!csv_path.empty()) {  // 本地模式导出全部命中（远程分支此前已返回）
    std::ofstream cf(csv_path);
    cf << "\"名称\",\"路径\",\"大小\",\"修改时间\",\"类型\"\n";
    for (kysearch::u32 i : res) {
      const auto& e = idx.entry(i);
      std::string p(idx.opath(i));
      size_t slash = p.find_last_of('/');
      cf << '\"' << p.substr(slash + 1) << "\",\"" << p.substr(0, slash)
         << "\",\"" << e.size << "\",\"" << fmt_time(e.mtime_sec)
         << "\",\"" << (e.type == kysearch::EntryType::File ? "文件" : "文件夹")
         << "\"\n";
    }
    std::printf("# 已导出 %zu 行 -> %s\n", res.size(), csv_path.c_str());
  }
  size_t shown = 0;
  for (kysearch::u32 i : res) {
    if (limit && shown >= limit) break;
    print_entry(idx, i);
    shown++;
  }
  std::cout << "# 命中 " << res.size() << " 项\n";

  if (bench) {
    // 基准：同一查询重复 K 次（每次清增量缓存，测真实查询路径）
    double best = 1e9, sum = 0;
    for (size_t k = 0; k < bench; ++k) {
      idx.cache_reset();
      auto t0 = std::chrono::steady_clock::now();
      std::vector<kysearch::u32> r;
      idx.search(q, r);
      double ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0).count();
      best = std::min(best, ms);
      sum += ms;
    }
    std::printf("# bench x%zu: min %.2f ms / avg %.2f ms\n", bench, best,
                sum / bench);
  }
  return 0;
}

// 打字基准：逐字符延伸查询词，对比冷查询与增量缓存路径
int cmd_bench_typing(std::vector<std::string>& args) {
  std::string db = default_db();
  std::string word;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--db") { if (i + 1 < args.size()) db = args[++i]; }
    else word = args[i];
  }
  if (word.empty()) { std::cerr << "用法: bench-typing [--db P] <词>\n"; return 2; }
  kysearch::FileIndex idx;
  if (!kysearch::FileIndex::load(db, idx)) {
    std::fprintf(stderr, "无法加载索引 %s\n", db.c_str());
    return 1;
  }
  std::printf("# 逐字符打字模拟：%s\n# len  冷查询ms  增量ms  命中\n", word.c_str());
  for (size_t len = 1; len <= word.size(); ++len) {
    std::string part = word.substr(0, len);
    auto q = kysearch::parse_query(part, {});
    idx.cache_reset();
    std::vector<kysearch::u32> r1;
    auto t0 = std::chrono::steady_clock::now();
    idx.search(q, r1);  // 冷（缓存被上一行的 reset 清空后的完整查询）
    double cold_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0).count();
    // 增量：先查前缀再延伸
    idx.cache_reset();
    if (len >= 2) {
      auto qp = kysearch::parse_query(word.substr(0, len - 1), {});
      std::vector<kysearch::u32> rp;
      idx.search(qp, rp);
    }
    std::vector<kysearch::u32> r2;
    auto t1 = std::chrono::steady_clock::now();
    idx.search(q, r2);
    double inc_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t1).count();
    std::printf("%5zu  %8.2f  %6.2f  %zu\n", len, cold_ms, inc_ms, r2.size());
  }
  return 0;
}

int cmd_info(std::vector<std::string>& args) {
  std::string db = default_db();
  for (size_t i = 0; i + 1 < args.size(); ++i)
    if (args[i] == "--db") { if (i + 1 < args.size()) db = args[++i]; }
  auto t0 = std::chrono::steady_clock::now();
  kysearch::FileIndex idx;
  if (!kysearch::FileIndex::load(db, idx)) {
    std::fprintf(stderr, "无法加载索引 %s\n", db.c_str());
    return 1;
  }
  auto ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
  auto st = idx.stats();
  std::printf("条目 %zu（扁平倒排 %zu 键 / %zu 指针，溢出 %zu，rank %s）\n",
              st.entry_count, st.gram_keys, st.gram_postings, st.gram_overflow,
              st.order_valid ? "有效" : "待重建");
  std::printf("内存池: lname %.1f MB, opath %.1f MB, pname %.1f MB\n",
              st.lname_bytes / 1048576.0, st.opath_bytes / 1048576.0,
              st.pname_bytes / 1048576.0);
  std::printf("快照加载 %.1f ms (%s)\n", ms, db.c_str());
  return 0;
}

int cmd_remote(const std::string& verb, std::vector<std::string>& args) {
  std::string socket = default_socket();
  for (size_t i = 0; i + 1 < args.size(); ++i)
    if (args[i] == "--socket") { if (i + 1 < args.size()) socket = args[++i]; }
  std::string out;
  if (!send_cmd(socket, verb, out)) {
    std::fprintf(stderr, "无法连接守护进程 %s\n", socket.c_str());
    return 1;
  }
  std::cout << out;
  return 0;
}

int cmd_trash(std::vector<std::string>& args) {
  if (args.empty()) { std::cerr << "用法: trash <path>...\n"; return 2; }
  int rc = 0;
  for (const auto& p : args) {
    std::string err;
    if (kysearch::trash_move(p, &err))
      std::printf("已移入回收站: %s\n", p.c_str());
    else {
      std::fprintf(stderr, "失败 %s: %s\n", p.c_str(), err.c_str());
      rc = 1;
    }
  }
  return rc;
}


// 重复文件查找：size 分组 → 首 64KB 哈希 → 全文件哈希 三级收敛
int cmd_dup(std::vector<std::string>& args) {
  std::string db = default_db();
  kysearch::u64 min_size = 1;
  size_t max_groups = 50;
  for (size_t i = 0; i < args.size(); ++i) {
    const auto& a = args[i];
    if (a == "--db") { if (i + 1 < args.size()) db = args[++i]; }
    else if (a == "--min-size") { if (i + 1 < args.size()) { bool pok = false; unsigned long long v = parse_ull(args[i + 1], pok); if (pok) { min_size = v; ++i; } } }
    else if (a == "--max-groups") { if (i + 1 < args.size()) { bool pok = false; unsigned long long v = parse_ull(args[i + 1], pok); if (pok) { max_groups = (size_t)v; ++i; } } }
  }
  kysearch::FileIndex idx;
  if (!kysearch::FileIndex::load(db, idx)) {
    std::fprintf(stderr, "无法加载索引 %s\n", db.c_str());
    return 1;
  }
  // 哈希 + 成功标志：不可读文件不再折进 0 桶误报为重复
  auto fnv_file = [](const std::string& p, kysearch::u64 cap,
                     bool& ok) -> kysearch::u64 {
    ok = false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return 0;
    ok = true;
    kysearch::u64 h = 14695981039346656037ull;
    char buf[65536];
    kysearch::u64 read_total = 0;
    while (f && read_total < cap) {
      f.read(buf, sizeof buf);
      std::streamsize n = f.gcount();
      if (n <= 0) break;
      for (std::streamsize i = 0; i < n; ++i) {
        h ^= (unsigned char)buf[i];
        h *= 1099511628211ull;
      }
      read_total += (kysearch::u64)n;
    }
    return h;
  };
  // 逐字节比对：哈希组在输出前做真值确认（FNV 冲突/构造碰撞防误删）
  auto bytes_equal = [](const std::string& a, const std::string& b) {
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    char ba[65536], bb[65536];
    for (;;) {
      fa.read(ba, sizeof ba);
      std::streamsize na = fa.gcount();
      fb.read(bb, sizeof bb);
      std::streamsize nb = fb.gcount();
      if (na != nb || std::memcmp(ba, bb, (size_t)na) != 0) return false;
      if (na == 0) return true;
    }
  };
  std::unordered_map<kysearch::u64, std::vector<std::string>> by_size;
  for (size_t i = 0; i < idx.entry_count(); ++i) {
    if (!idx.alive((kysearch::u32)i)) continue;
    const auto& e = idx.entry((kysearch::u32)i);
    if (e.type != kysearch::EntryType::File || e.size < min_size) continue;
    by_size[e.size].push_back(std::string(idx.opath((kysearch::u32)i)));
  }
  size_t groups = 0;
  for (const auto& kv : by_size) {
    if (kv.second.size() < 2) continue;
    std::unordered_map<kysearch::u64, std::vector<std::string>> by_head;
    for (const auto& p : kv.second) {
      bool pok = false;
      kysearch::u64 h = fnv_file(p, 65536, pok);
      if (pok) by_head[h].push_back(p);  // 不可读：跳过而非入 0 桶
    }
    for (const auto& hh : by_head) {
      if (hh.second.size() < 2) continue;
      std::unordered_map<kysearch::u64, std::vector<std::string>> by_full;
      for (const auto& p : hh.second) {
        bool pok = false;
        kysearch::u64 h = fnv_file(p, UINT64_MAX, pok);
        if (pok) by_full[h].push_back(p);
      }
      for (const auto& ff : by_full) {
        if (ff.second.size() < 2) continue;
        // 与组内首个文件逐字节确认（哈希碰撞防误报）
        std::vector<std::string> verified;
        for (size_t vi = 0; vi < ff.second.size(); ++vi)
          if (vi == 0 || bytes_equal(ff.second[0], ff.second[vi]))
            verified.push_back(ff.second[vi]);
        if (verified.size() < 2) continue;
        std::printf("重复组 #%zu：大小 %s，%zu 个相同文件（已逐字节确认）\n",
                    ++groups, human_size(kv.first).c_str(), verified.size());
        for (const auto& p : verified) std::printf("    %s\n", p.c_str());
        if (groups >= max_groups) {
          std::printf("…（--max-groups 上限 %zu，已截断）\n", max_groups);
          return 0;
        }
      }
    }
  }
  if (!groups)
    std::printf("未发现重复文件（min-size=%llu）\n",
                (unsigned long long)min_size);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "kysearch-cli " << KYSEARCH_VERSION_STR
              << "\n用法:\n  kysearch-cli scan <root>... [--db P] [--threads N] [--progress]\n"
                 "  kysearch-cli search [--db P | --socket P] [--path] [--case] [--word] [--regex]\n"
                 "                      [--limit N] [--bench K] <terms...>\n"
                 "  kysearch-cli info [--db P]\n"
                 "  kysearch-cli status [--socket P]   # 查看守护进程状态\n"
                 "  kysearch-cli stop   [--socket P]   # 通知守护进程落盘退出\n";
    return 2;
  }
  std::string cmd = argv[1];
  std::vector<std::string> args(argv + 2, argv + argc);
  if (cmd == "scan") return cmd_scan(args);
  if (cmd == "search") return cmd_search(args);
  if (cmd == "info") return cmd_info(args);
  if (cmd == "bench-typing") return cmd_bench_typing(args);
  if (cmd == "status") return cmd_remote("STATUS", args);
  if (cmd == "stop") return cmd_remote("STOP", args);
  if (cmd == "rescan") return cmd_remote("RESCAN", args);
  if (cmd == "setroots") {  // 热更新索引根：setroots /pathA /pathB ...
    if (args.empty() || args[0].rfind("--", 0) == 0) {
      std::fprintf(stderr, "用法: kysearch-cli setroots <绝对路径>...（全量覆盖现有根）\n");
      return 2;
    }
    std::string req = "SETROOTS";
    for (size_t si = 0; si < args.size(); ++si) {
      if (args[si] == "--socket") { ++si; continue; }  // 连同取值一起跳过
      req += "\t" + args[si];
    }
    return cmd_remote(req, args);
  }
  if (cmd == "trash") return cmd_trash(args);
  if (cmd == "dup") return cmd_dup(args);
  std::fprintf(stderr, "未知命令: %s\n", cmd.c_str());
  return 2;
}
