// 核心引擎单元测试 v2：扫描计数 / 搜索模式 / 过滤器语法 / 正则 / 增量更新 / 快照往返
#include <cstdio>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "core/index.h"
#include "core/pinyin.h"
#include "core/fsutil.h"
#include "core/scanner.h"

namespace fs = std::filesystem;
using kysearch::FileIndex;
using kysearch::Query;
using kysearch::QueryFlags;
using kysearch::ScanOptions;
using kysearch::Scanner;

#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::cerr << "FAIL 第 " << __LINE__ << " 行: " #cond "\n"; \
      return 1;                                                  \
    }                                                            \
  } while (0)
#define CHECK2(cond, msg)                                        \
  do {                                                           \
    if (!(cond)) {                                               \
      std::cerr << "FAIL 第 " << __LINE__ << " 行: " << (msg)    \
                << " (" #cond ")\n";                             \
      return 1;                                                  \
    }                                                            \
  } while (0)

static size_t hits(const FileIndex& idx, const std::string& raw,
                   QueryFlags f = {}) {
  Query q = kysearch::parse_query(raw, f);
  std::vector<kysearch::u32> r;
  idx.search(q, r);
  return r.size();
}

int main() {

  fs::path base = fs::temp_directory_path() / "kysearch-unit";
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base / "A");
  fs::create_directories(base / "B");
  fs::create_directories(base / "C");
  fs::create_directories(base / "D/nested/deep");
  auto touch = [](const fs::path& p, const std::string& data) {
    std::ofstream(p) << data;
  };
  touch(base / "A/Readme.TXT", "x");
  touch(base / "A/abc.txt", "hello");
  touch(base / "B/ABC notes.md", "x");
  touch(base / "B/weekly report 001.md", "x");
  touch(base / "C/报告_2024.txt", "x");
  touch(base / "C/手机001.jpg", "x");
  touch(base / "C/银行.txt", "x");
  touch(base / "D/nested/智能设备.bin", "x");
  touch(base / "D/nested/deep/data.bin", std::string(2048, 'z'));  // 2KB 大文件

  FileIndex idx;
  ScanOptions opt;
  opt.roots = {base.string()};
  opt.threads = 2;
  opt.pinyin_fn = kysearch::pinyin_key;  // 拼音索引
  Scanner sc(idx);
  sc.run(opt);

  // 7 个文件 + 2 个模糊音用例文件 + 6 个目录（A/B/C/D/nested/deep）= 15
  CHECK(idx.entry_count() == 15);
  CHECK(idx.alive_count() == 15);

  // ---- 基础搜索 ----
  QueryFlags cs;   // 区分大小写
  cs.case_sensitive = true;
  QueryFlags ww;   // 全字匹配
  ww.whole_word = true;
  QueryFlags pm;   // 路径模式
  pm.match_path = true;
  CHECK(hits(idx, "abc") == 2);        // abc.txt + ABC notes.md
  CHECK(hits(idx, "abc", cs) == 1);    // 仅 abc.txt
  CHECK(hits(idx, "ABC", cs) == 1);    // ABC notes.md
  CHECK(hits(idx, "report", ww) == 1);
  CHECK(hits(idx, "nested", pm) == 4);
  CHECK(hits(idx, "nested") == 1);     // 仅 nested 目录本身
  CHECK(hits(idx, "报告 txt") == 1);   // AND
  CHECK(hits(idx, "手机") == 1);       // 中文子串

  // ---- 拼音搜索（全拼 / 首字母）----
  CHECK(hits(idx, "shouji") == 1);     // 手机001.jpg
  CHECK(hits(idx, "sj") == 1);         // 首字母
  CHECK(hits(idx, "baogao") == 1);     // 报告_2024.txt
  CHECK(hits(idx, "bg") == 1);
  CHECK(hits(idx, "shouji001") == 1);  // 拼音+数字混合
  CHECK(hits(idx, "shouji", ww) == 0); // 全字模式不参与拼音（by design）

  // ---- 拼音模糊音（z↔zh c↔ch s↔sh n↔l f↔h）----
  CHECK(hits(idx, "yinhang") == 1);   // 银行
  CHECK(hits(idx, "yinfang") == 1);   // h→f 模糊
  CHECK(hits(idx, "zhineng") == 1);   // 智能
  CHECK(hits(idx, "zineng") == 1);    // zh→z 模糊
  CHECK(hits(idx, "zhileng") == 1);   // n→l 模糊（能 neng→leng）

  CHECK(hits(idx, "") == 15);          // 空查询 = 全部

  // ---- 过滤器语法 ----
  CHECK(hits(idx, "ext:txt") == 4);             // abc.txt + 报告_2024.txt + Readme.TXT + 银行.txt
  CHECK(hits(idx, "ext:md,txt") == 6);
  CHECK(hits(idx, "type:dir") == 6);
  CHECK(hits(idx, "type:file") == 9);
  CHECK(hits(idx, "size:>100b") == 1);          // data.bin (2KB)
  CHECK(hits(idx, "size:2048b") == 1);
  CHECK(hits(idx, "size:<100b type:file") == 8);
  CHECK(hits(idx, "dm:today") == 15);           // 刚创建
  CHECK(hits(idx, "dm:2020-01-01") == 0);
  CHECK(hits(idx, "path:c/") == 3);             // C 目录下 3 个文件（目录自身无尾斜杠不匹配）
  CHECK(hits(idx, "txt type:file dm:today") == 4);

  // ---- 解析器：content: 词不进 terms ----
  {
    auto q = kysearch::parse_query("验收 content:里程碑 ext:docx", {});
    CHECK(q.terms.size() == 1 && q.terms[0] == "验收");
    CHECK(q.content_terms.size() == 1 && q.content_terms[0] == "里程碑");
    CHECK(q.filt.has_ext && q.filt.exts[0] == "docx");
  }

  // ---- 正则（RE2）----
#ifdef KYSEARCH_HAVE_RE2
  QueryFlags rx;
  rx.regex = true;
  CHECK(hits(idx, "^abc", rx) == 1);        // 仅 abc.txt（ABC notes.md 大写开头）
  CHECK(hits(idx, "(?i)^abc", rx) == 2);    // 忽略大小写前缀
  CHECK(hits(idx, "^data\\.(bin|txt)$", rx) == 1);
  CHECK(hits(idx, "[0-9]{3}", rx) == 3);        // 手机001 / 报告_2024 / report 001
  QueryFlags rxp;   // 正则 + 路径模式
  rxp.regex = true;
  rxp.match_path = true;
  CHECK(hits(idx, "A/", rxp) == 2);             // A 目录下两个文件（原始大小写路径）
#endif

  // ---- 增量更新 ----
  std::string newfile = (base / "A/newfile.log").string();
  idx.upsert(newfile, 10, std::time(nullptr), kysearch::EntryType::File);
  CHECK(idx.alive_count() == 16);
  CHECK(hits(idx, "newfile") == 1);
  CHECK(idx.remove_by_path(newfile));
  CHECK(idx.alive_count() == 15);
  CHECK(hits(idx, "newfile") == 0);
  CHECK(!idx.remove_by_path(newfile));  // 重复删除返回 false

  // 改写已有文件（upsert 同路径）：旧条目打墓碑，新条目生效
  std::string abc = (base / "A/abc.txt").string();
  idx.upsert(abc, 99, std::time(nullptr), kysearch::EntryType::File);
  CHECK(idx.alive_count() == 15);
  {
    Query q = kysearch::parse_query("abc", {});
    std::vector<kysearch::u32> r;
    idx.search(q, r);
    CHECK(r.size() == 2);
    for (auto i : r)
      if (idx.opath(i) == abc) CHECK(idx.entry(i).size == 99);
  }

  // ---- 快照往返（v3，含增量墓碑） ----
  fs::path db = base / "snap.bin";
  CHECK(idx.save(db.string()));
  CHECK(!fs::exists(db.string() + ".tmp"));  // 原子落盘无残留
  FileIndex idx2;
  CHECK(FileIndex::load(db.string(), idx2));
  CHECK(idx2.entry_count() == idx.entry_count());
  CHECK(idx2.alive_count() == 15);
  {
    std::vector<kysearch::u32> r1, r2;
    idx.search(kysearch::parse_query("txt", {}), r1);
    idx2.search(kysearch::parse_query("txt", {}), r2);
    CHECK(r1 == r2);
    CHECK(r2.size() == 4);
  }

  // ---- 布尔运算（v3）----
  CHECK(hits(idx, "报告 | 手机") == 2);
  CHECK(hits(idx, "!报告") == 14);           // 15 - 报告_2024.txt
  CHECK(hits(idx, "txt !readme") == 3);      // 排除 Readme.TXT
  CHECK(hits(idx, "(报告 | 手机) ext:txt") == 1);
  CHECK(hits(idx, "手机 | 报告 | abc") == 4);
  CHECK(hits(idx, "(deep | data) ext:bin") == 1);

  // ---- 排序不变式：rank 与字符串回退两条路径均须名序非降 ----
  auto assert_sorted = [&](const char* what) -> int {
    std::vector<kysearch::u32> r;
    idx.search(kysearch::parse_query("", {}), r);
    for (size_t i = 1; i < r.size(); ++i) {
      auto a = idx.lname(r[i - 1]), b = idx.lname(r[i]);
      CHECK2(a <= b, what);
    }
    return 0;
  };
  assert_sorted("finalize 后 rank 排序");
  idx.upsert((base / "A/zzz新文件.log").string(), 1, std::time(nullptr),
             kysearch::EntryType::File);
  assert_sorted("upsert 后字符串回退排序");
  idx.finalize();
  assert_sorted("再 finalize 后 rank 排序");

  // ---- 路径子树模式 vs 暴力对照 ----
  {
    kysearch::QueryFlags pf;
    pf.match_path = true;
    for (const char* t : {"nested", "c", "d", "deep", "nested/deep", "a"}) {
      std::vector<kysearch::u32> got;
      idx.search(kysearch::parse_query(t, pf), got);
      std::vector<std::string> got_paths;
      for (auto i : got) got_paths.push_back(std::string(idx.opath(i)));
      std::sort(got_paths.begin(), got_paths.end());
      std::vector<std::string> want;
      for (size_t i = 0; i < idx.entry_count(); ++i) {
        if (!idx.alive((kysearch::u32)i)) continue;
        std::string lp(idx.opath((kysearch::u32)i));
        for (char& c : lp)
          if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if (lp.find(t) != std::string::npos)
          want.push_back(std::string(idx.opath((kysearch::u32)i)));
      }
      std::sort(want.begin(), want.end());
      CHECK2(got_paths == want, t);
    }
  }

  // ---- 增量缓存：延伸查询与冷查询结果一致 ----
  {
    idx.cache_reset();
    std::vector<kysearch::u32> warm;
    idx.search(kysearch::parse_query("shou", {}), warm);   // 建缓存
    idx.search(kysearch::parse_query("shouji", {}), warm); // 命中缓存路径
    std::vector<kysearch::u32> cold;
    idx.cache_reset();
    idx.search(kysearch::parse_query("shouji", {}), cold); // 冷路径
    CHECK2(warm == cold, "缓存与冷查询一致");
    CHECK(warm.size() == 1);
  }

  // ---- 回收站删除（freedesktop 协议，测试专用 trash 目录）----
  {
    fs::path trash_base = base / "trash";
    fs::path victim = base / "A/待删除文件.txt";
    { std::ofstream(victim) << "x"; }
    std::string err;
    CHECK(kysearch::trash_move(victim.string(), &err, trash_base.string()));
    CHECK(!fs::exists(victim));
    CHECK(fs::exists(trash_base / "files" / "待删除文件.txt"));
    CHECK(fs::exists(trash_base / "info" / "待删除文件.txt.trashinfo"));
    // 不存在的文件报错
    CHECK(!kysearch::trash_move(victim.string(), &err, trash_base.string()));
  }

  fs::remove_all(base, ec);
  std::cout << "ALL CORE TESTS PASSED\n";
  return 0;
}
