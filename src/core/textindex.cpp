#include "core/textindex.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <unordered_set>

#include "core/index.h"

namespace kysearch {

namespace {

bool has_ext(const std::string& path, const char* const* exts, size_t n) {
  size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return false;
  std::string ext = path.substr(dot + 1);
  for (char c : ext) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  }
  for (size_t i = 0; i < n; ++i)
    if (ext == exts[i]) return true;
  return false;
}

std::string sql_like_escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '%' || c == '_' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

}  // namespace

TextIndex::~TextIndex() {
  if (db_) sqlite3_close(db_);
}

bool TextIndex::open(const std::string& db_path) {
  if (db_) {  // 重复 open：先关旧连接，防句柄泄漏与读旧库
    std::lock_guard<std::mutex> g(mtx_);
    sqlite3_close(db_);
    db_ = nullptr;
  }
  if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
    sqlite3_close(db_);
    db_ = nullptr;
    return false;
  }
  sqlite3_busy_timeout(db_, 3000);
  char* err = nullptr;
  // trigram 分词器：内容子串检索（SQLite >= 3.34）。
  // 麒麟 V10/focal 基线是 SQLite 3.31（无 trigram）：降级为 unicode61
  // 建表 + 全词 LIKE 检索（语义一致、无相关性排序），不再整体禁用。
  if (sqlite3_exec(db_,
                   "CREATE VIRTUAL TABLE IF NOT EXISTS fts USING fts5("
                   "path UNINDEXED, content, tokenize='trigram');",
                   nullptr, nullptr, &err) != SQLITE_OK) {
    if (err) sqlite3_free(err);
    err = nullptr;
    trigram_ = false;
    if (sqlite3_exec(db_,
                     "CREATE VIRTUAL TABLE IF NOT EXISTS fts USING fts5("
                     "path UNINDEXED, content, tokenize='unicode61');",
                     nullptr, nullptr, &err) != SQLITE_OK) {
      std::fprintf(stderr, "kysearch: FTS5 不可用: %s\n", err ? err : "?");
      if (err) sqlite3_free(err);
      sqlite3_close(db_);
      db_ = nullptr;
      return false;
    }
    std::fprintf(stderr,
                 "kysearch: SQLite 无 trigram（<3.34），全文检索降级为 LIKE 模式\n");
  } else {
    trigram_ = true;
  }
  return true;
}

bool TextIndex::want_file(const std::string& path, u64 max_bytes, u64 file_size) {
  static const char* const kExts[] = {"txt", "md",  "log",  "csv", "json",
                                      "xml", "html", "htm", "ini", "conf",
                                      "yaml", "yml"};
  if (!has_ext(path, kExts, sizeof kExts / sizeof kExts[0])) return false;
  return file_size <= max_bytes;
}

bool TextIndex::index_file(const std::string& path) {
  if (!db_) return false;
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  // 读取阶段即限量（原整文件读入再截断，大日志会瞬时内存暴涨）
  std::string content;
  content.resize((size_t)kDefaultMaxBytes);
  f.read(content.data(), content.size());
  content.resize((size_t)f.gcount());
  // 截断点回退到 UTF-8 边界：先剥续字节，再剥孤立的前导字节
  while (!content.empty() && ((unsigned char)content.back() & 0xC0) == 0x80)
    content.pop_back();
  if (!content.empty() && (unsigned char)content.back() >= 0xC0)
    content.pop_back();  // 前导字节后无完整序列：一并丢弃

  std::lock_guard<std::mutex> g(mtx_);
  char* err = nullptr;
  sqlite3_exec(db_, "BEGIN", nullptr, nullptr, &err);
  sqlite3_stmt* del = nullptr;
  bool ok = false;
  if (sqlite3_prepare_v2(db_, "DELETE FROM fts WHERE path = ?", -1, &del,
                         nullptr) == SQLITE_OK) {
    sqlite3_bind_text(del, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(del);
    sqlite3_finalize(del);
    sqlite3_stmt* ins = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT INTO fts(path, content) VALUES(?, ?)",
                           -1, &ins, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(ins, 1, path.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(ins, 2, content.data(), (int)content.size(),
                        SQLITE_TRANSIENT);
      ok = sqlite3_step(ins) == SQLITE_DONE;
      sqlite3_finalize(ins);
    }
  }
  const int rc_fin =
      sqlite3_exec(db_, ok ? "COMMIT" : "ROLLBACK", nullptr, nullptr, &err);
  if (err) sqlite3_free(err);
  return ok && rc_fin == SQLITE_OK;  // COMMIT 失败（忙/磁盘满）不算成功
}

void TextIndex::remove_file(const std::string& path) {
  if (!db_) return;
  std::lock_guard<std::mutex> g(mtx_);
  sqlite3_stmt* del = nullptr;
  sqlite3_prepare_v2(db_, "DELETE FROM fts WHERE path = ?", -1, &del, nullptr);
  sqlite3_bind_text(del, 1, path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(del);
  sqlite3_finalize(del);
}

size_t TextIndex::count() const {
  if (!db_) return 0;
  std::lock_guard<std::mutex> g(mtx_);
  sqlite3_stmt* st = nullptr;
  sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM fts", -1, &st, nullptr);
  size_t n = 0;
  if (sqlite3_step(st) == SQLITE_ROW) n = (size_t)sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return n;
}

std::vector<std::string> TextIndex::search(const std::vector<std::string>& terms,
                                           size_t limit) const {
  std::vector<std::string> out;
  if (!db_ || terms.empty()) return out;
  std::lock_guard<std::mutex> g(mtx_);

  // trigram 阈值按 UTF-8 字符数（非字节）：单中文字（3 字节=1 字符）、
  // 双字词（6 字节=2 字符）都无法产出 trigram token，必须走 LIKE；
  // 否则 MATCH 静默返回空。仅 >=3 字符的词进 MATCH。
  auto char_count = [](const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
      if ((c & 0xC0) != 0x80) ++n;  // 非 UTF-8 续字节计一个字符
    return n;
  };
  std::string match_expr;
  std::vector<std::string> likes;
  for (const auto& t : terms) {
    std::string esc;
    for (char c : t)
      if (c == '"') esc += "\"\"";
      else esc += c;
    if (trigram_ && char_count(t) >= 3) {
      if (!match_expr.empty()) match_expr += " AND ";
      match_expr += "\"" + esc + "\"";
    } else {
      likes.push_back(t);  // 无 trigram 模式：全部词走 LIKE（子串语义一致）
    }
  }
  if (match_expr.empty() && likes.empty()) return out;

  std::string sql = "SELECT path FROM fts";
  std::vector<std::string> binds;
  if (!match_expr.empty()) sql += " WHERE fts MATCH ?";
  size_t li = 0;
  for (const auto& t : likes) {
    sql += match_expr.empty() && li == 0 ? " WHERE" : " AND";
    sql += " content LIKE ? ESCAPE '\\'";
    ++li;
  }
  // bm25 仅在存在 MATCH 时可用（否则 prepare 报错、静默空结果）
  sql += match_expr.empty() ? " LIMIT " : " ORDER BY bm25(fts) LIMIT ";
  sql += std::to_string(limit);

  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    sqlite3_finalize(st);
    return out;
  }
  int col = 1;
  if (!match_expr.empty())
    sqlite3_bind_text(st, col++, match_expr.c_str(), -1, SQLITE_TRANSIENT);
  for (const auto& t : likes) {
    std::string pat = "%" + sql_like_escape(t) + "%";
    sqlite3_bind_text(st, col++, pat.c_str(), -1, SQLITE_TRANSIENT);
  }
  while (sqlite3_step(st) == SQLITE_ROW) {
    const unsigned char* p = sqlite3_column_text(st, 0);
    if (p) out.emplace_back((const char*)p);
  }
  sqlite3_finalize(st);
  return out;
}

void apply_content_filter(const TextIndex& t, const Query& q,
                          const FileIndex& idx, std::vector<u32>& res) {
  if (q.content_terms.empty()) return;
  if (!t.ok()) {
    res.clear();  // 内容索引不可用：内容查询无意义
    return;
  }
  auto paths = t.search(q.content_terms);
  std::unordered_set<std::string> hit(paths.begin(), paths.end());
  std::vector<u32> filtered;
  filtered.reserve(res.size());
  for (u32 i : res)
    if (hit.count(std::string(idx.opath(i)))) filtered.push_back(i);
  res = std::move(filtered);
}

}  // namespace kysearch
