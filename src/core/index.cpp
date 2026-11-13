#include "core/index.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <type_traits>

#ifdef KYSEARCH_HAVE_RE2
#include <re2/re2.h>
#endif

namespace kysearch {

namespace {

inline char ascii_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

inline std::string to_lower(std::string s) {
  for (auto& c : s) c = ascii_lower(c);
  return s;
}

inline bool is_word_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

bool contains(std::string_view hay, std::string_view needle, bool whole_word) {
  if (needle.empty()) return true;
  size_t pos = 0;
  while ((pos = hay.find(needle, pos)) != std::string_view::npos) {
    if (!whole_word) return true;
    bool left_ok = pos == 0 || !is_word_char(hay[pos - 1]);
    size_t right = pos + needle.size();
    bool right_ok = right >= hay.size() || !is_word_char(hay[right]);
    if (left_ok && right_ok) return true;
    ++pos;
  }
  return false;
}

inline u32 gram_key(unsigned char a, unsigned char b) { return u32(a) << 8 | b; }

u32 append_arena(std::vector<char>& arena, std::string_view s) {
  u32 off = (u32)arena.size();
  arena.insert(arena.end(), s.begin(), s.end());
  return off;
}

std::string_view last_component(std::string_view path) {
  size_t p = path.find_last_of('/');
  return p == std::string_view::npos ? path : path.substr(p + 1);
}

// 零分配的大小写不敏感包含（needle 需已小写）
bool contains_ci(std::string_view hay, std::string_view needle_lower,
                 bool whole_word) {
  const size_t n = needle_lower.size();
  if (n == 0) return true;
  if (hay.size() < n) return false;
  for (size_t pos = 0; pos + n <= hay.size(); ++pos) {
    size_t k = 0;
    while (k < n && ascii_lower(hay[pos + k]) == needle_lower[k]) ++k;
    if (k < n) continue;
    if (!whole_word) return true;
    bool lok = pos == 0 || !is_word_char(hay[pos - 1]);
    size_t r = pos + n;
    bool rok = r >= hay.size() || !is_word_char(hay[r]);
    if (lok && rok) return true;
  }
  return false;
}

bool path_eq_ci(std::string_view opath, std::string_view lp_lower) {
  if (opath.size() != lp_lower.size()) return false;
  for (size_t i = 0; i < opath.size(); ++i)
    if (ascii_lower(opath[i]) != lp_lower[i]) return false;
  return true;
}

bool ci_starts_with(std::string_view opath, std::string_view p_lower) {
  if (opath.size() < p_lower.size()) return false;
  for (size_t i = 0; i < p_lower.size(); ++i)
    if (ascii_lower(opath[i]) != p_lower[i]) return false;
  return true;
}

bool flags_eq(const QueryFlags& a, const QueryFlags& b) {
  return a.case_sensitive == b.case_sensitive && a.whole_word == b.whole_word &&
         a.match_path == b.match_path && a.regex == b.regex;
}
bool filters_eq(const Filters& a, const Filters& b) {
  return a.has_ext == b.has_ext && a.exts == b.exts &&
         a.size_cmp == b.size_cmp && a.size_bytes == b.size_bytes &&
         a.has_dm == b.has_dm && a.dm_from == b.dm_from && a.dm_to == b.dm_to &&
         a.type_filter == b.type_filter && a.has_path_sub == b.has_path_sub &&
         a.path_sub == b.path_sub;
}

// ---- 过滤器值解析（同 v2）----

bool parse_size_value(const std::string& s, u64& bytes) {
  char* end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  if (end == s.c_str() || v < 0) return false;
  std::string unit = to_lower(std::string(end));
  double mul = 1;
  if (unit.empty() || unit == "b") mul = 1;
  else if (unit == "kb" || unit == "k") mul = 1024;
  else if (unit == "mb" || unit == "m") mul = 1024 * 1024;
  else if (unit == "gb" || unit == "g") mul = 1024.0 * 1024 * 1024;
  else if (unit == "tb" || unit == "t") mul = 1024.0 * 1024 * 1024 * 1024;
  else return false;
  bytes = (u64)(v * mul);
  return true;
}

bool parse_day(const std::string& s, std::int64_t& sec) {
  int y, m, d;
  if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return false;
  if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return false;
  std::tm tmv{};
  tmv.tm_year = y - 1900;
  tmv.tm_mon = m - 1;
  tmv.tm_mday = d;
  tmv.tm_isdst = -1;
  time_t t = std::mktime(&tmv);
  if (t == (time_t)-1) return false;
  sec = (std::int64_t)t;
  return true;
}

std::tm local_now() {
  time_t t = std::time(nullptr);
  std::tm tmv{};
  localtime_r(&t, &tmv);
  return tmv;
}

time_t tm_to_time(std::tm tmv) {
  tmv.tm_hour = 0;
  tmv.tm_min = 0;
  tmv.tm_sec = 0;
  tmv.tm_isdst = -1;
  return std::mktime(&tmv);
}

bool parse_dm_value(const std::string& v, std::int64_t& from, std::int64_t& to) {
  std::string s = to_lower(v);
  std::tm n = local_now();
  if (s == "today") { from = tm_to_time(n); to = from + 86400; return true; }
  if (s == "yesterday") { n.tm_mday -= 1; from = tm_to_time(n); to = from + 86400; return true; }
  if (s == "thisweek") {
    int dsm = (n.tm_wday + 6) % 7;
    n.tm_mday -= dsm;
    from = tm_to_time(n);
    to = from + 7 * 86400;
    return true;
  }
  if (s == "thismonth") {
    n.tm_mday = 1;
    from = tm_to_time(n);
    std::tm nx = n;
    nx.tm_mon += 1;
    to = tm_to_time(nx);
    return true;
  }
  if (s.size() >= 2 && s.back() == 'd' &&
      s.find_first_not_of("0123456789", 0) == s.size() - 1) {
    int days = std::atoi(s.c_str());
    if (days <= 0) return false;
    to = std::time(nullptr) + 86400;
    from = to - (std::int64_t)days * 86400;
    return true;
  }
  size_t dots = s.find("..");
  if (dots != std::string::npos) {
    std::int64_t a, b;
    if (!parse_day(v.substr(0, dots), a) || !parse_day(v.substr(dots + 2), b))
      return false;
    from = a;
    to = b + 86400;
    return true;
  }
  if (parse_day(v, from)) { to = from + 86400; return true; }
  return false;
}

bool is_date_token(const std::string& v) {
  return v.find('-') != std::string::npos ||
         (!v.empty() && v.back() == 'd' &&
          v.find_first_not_of("0123456789") == v.size() - 1) ||
         to_lower(v) == "today" || to_lower(v) == "yesterday" ||
         to_lower(v) == "thisweek" || to_lower(v) == "thismonth";
}

// ---- 布尔词法 + 递归下降 ----

enum TokType { T_LP, T_RP, T_OR, T_NOT, T_WORD };
struct Tok {
  TokType t;
  std::string s;
};

void classify_word(const std::string& w, Query& q, std::vector<Tok>& toks) {
  size_t colon = w.find(':');
  if (colon == std::string::npos) {
    toks.push_back(Tok{T_WORD, w});
    return;
  }
  std::string key = to_lower(w.substr(0, colon));
  std::string val = w.substr(colon + 1);
  if (key == "ext" && !val.empty()) {
    q.filt.has_ext = true;
    std::string v = to_lower(val);
    if (!v.empty() && v[0] == '.') v = v.substr(1);
    size_t start = 0;
    while (start <= v.size()) {
      size_t comma = v.find(',', start);
      std::string one = v.substr(start, comma == std::string::npos
                                             ? std::string::npos
                                             : comma - start);
      if (!one.empty()) q.filt.exts.push_back(one);
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
  } else if (key == "size" && !val.empty()) {
    const char* p = val.c_str();
    int cmp = 5;
    if (*p == '>' || *p == '<') {
      cmp = (*p == '>') ? 1 : 3;
      ++p;
      if (*p == '=') { cmp = (cmp == 1) ? 2 : 4; ++p; }
    } else if (*p == '=') {
      ++p;
    }
    if (parse_size_value(p, q.filt.size_bytes)) q.filt.size_cmp = cmp;
  } else if (key == "dm" && !val.empty() && is_date_token(val)) {
    std::int64_t f, t;
    if (parse_dm_value(val, f, t)) {
      q.filt.has_dm = true;
      q.filt.dm_from = f;
      q.filt.dm_to = t;
    }
  } else if (key == "type" &&
             (to_lower(val) == "file" || to_lower(val) == "dir")) {
    q.filt.type_filter = to_lower(val) == "file" ? 1 : 2;
  } else if (key == "path" && !val.empty()) {
    q.filt.has_path_sub = true;
    q.filt.path_sub = to_lower(val);
  } else if (key == "content" && !val.empty()) {
    q.content_terms.push_back(val);
  } else {
    toks.push_back(Tok{T_WORD, w});  // 冒号词当普通关键词
  }
}

struct Parser {
  const std::vector<Tok>& toks;
  size_t pos = 0;
  Query& q;
  bool ok = true;
  int depth = 0;  // 嵌套深度守卫（恶意多括号防栈溢出）

  bool peek_is(TokType t) const { return pos < toks.size() && toks[pos].t == t; }

  QNode parse_expr() {
    QNode left = parse_and();
    if (!peek_is(T_OR)) return left;
    QNode node;
    node.kind = QNode::Or;
    node.kids.push_back(std::move(left));
    while (peek_is(T_OR)) {
      ++pos;
      node.kids.push_back(parse_and());
    }
    return node;
  }

  QNode parse_and() {
    std::vector<QNode> ks;
    while (peek_is(T_LP) || peek_is(T_NOT) || peek_is(T_WORD)) {
      if (ks.size() >= 16) { ok = false; break; }  // 防深防炸
      ks.push_back(parse_unary());
    }
    if (ks.empty()) { ok = false; return QNode{}; }
    if (ks.size() == 1) return std::move(ks[0]);
    QNode n;
    n.kind = QNode::And;
    n.kids = std::move(ks);
    return n;
  }

  QNode parse_unary() {
    struct DepthGuard {
      int& d;
      explicit DepthGuard(int& x) : d(++x) {}
      ~DepthGuard() { --d; }
    } dg(depth);
    if (depth > 64) {
      ok = false;
      return QNode{};
    }
    if (peek_is(T_NOT)) {
      ++pos;
      if (pos >= toks.size()) {  // 尾随 '!'：词法合法但无操作数
        ok = false;
        return QNode{};
      }
      QNode n;
      n.kind = QNode::Not;
      n.kids.push_back(parse_unary());
      return n;
    }
    if (peek_is(T_LP)) {
      ++pos;
      QNode e = parse_expr();
      if (peek_is(T_RP)) ++pos;
      else ok = false;
      return e;
    }
    if (pos >= toks.size()) {  // 容错：不应到达
      ok = false;
      return QNode{};
    }
    // T_WORD
    QNode n;
    n.kind = QNode::Term;
    n.term = (int)q.terms.size();
    q.terms.push_back(toks[pos].s);
    ++pos;
    return n;
  }
};

}  // namespace

Query parse_query(const std::string& raw, QueryFlags flags) {
  Query q;
  q.raw = raw;
  q.flags = flags;

  std::vector<Tok> toks;
  std::string cur;
  auto flush = [&] {
    if (!cur.empty()) {
      classify_word(cur, q, toks);
      cur.clear();
    }
  };
  for (char c : raw) {
    switch (c) {
      case ' ': case '\t': case '\n': case '\r': flush(); break;
      case '(': flush(); toks.push_back(Tok{T_LP, ""}); break;
      case ')': flush(); toks.push_back(Tok{T_RP, ""}); break;
      case '|': flush(); toks.push_back(Tok{T_OR, ""}); break;
      case '!': flush(); toks.push_back(Tok{T_NOT, ""}); break;
      default: cur.push_back(c);
    }
  }
  flush();

  Parser p{toks, 0, q, true};
  if (!toks.empty()) q.root = p.parse_expr();
  // 容错：多余符号丢弃，解析失败退化为 AND 全词
  if (!p.ok) {
    q.root = QNode{};
    q.terms.clear();
    for (const auto& t : toks)
      if (t.t == T_WORD) q.terms.push_back(t.s);
    if (!q.terms.empty()) {
      q.root.kind = QNode::And;
      for (size_t i = 0; i < q.terms.size(); ++i)
        q.root.kids.push_back(QNode{QNode::Term, (int)i, {}});
    }
  }

  // 全正词 AND（快速路径 + 增量缓存适用）
  q.flat_and = !q.terms.empty() && q.root.kind == QNode::And &&
               q.root.kids.size() == q.terms.size();
  if (q.flat_and)
    for (const auto& k : q.root.kids)
      if (k.kind != QNode::Term) q.flat_and = false;
  return q;
}

// ---- FileIndex ----

u64 FileIndex::hash_ci(std::string_view opath) {
  u64 h = 14695981039346656037ull;  // FNV-1a 64 标准偏移基（原缺末位 7，仅影响哈希分布不影响正确性；by_lpath_ 不持久化，改值无兼容问题）
  for (char c : opath) {
    h ^= (unsigned char)ascii_lower(c);
    h *= 1099511628211ull;
  }
  return h;
}

u32 FileIndex::add(std::string_view lname, std::string_view opath,
                   std::string_view pname, u64 size, std::int64_t mtime,
                   EntryType type) {
  std::unique_lock<std::shared_mutex> g(mtx_);
  return relock_add(lname, opath, pname, size, mtime, type);
}

u32 FileIndex::relock_add(std::string_view lname, std::string_view opath,
                          std::string_view pname, u64 size,
                          std::int64_t mtime, EntryType type) {
  Entry e;
  e.lname_off = append_arena(lname_arena_, lname);
  e.lname_len = (u32)lname.size();
  e.opath_off = append_arena(opath_arena_, opath);
  e.opath_len = (u32)opath.size();
  e.pname_off = append_arena(pname_arena_, pname);
  e.pname_len = (u32)pname.size();
  e.size = size;
  e.mtime_sec = mtime;
  e.type = type;
  entries_.push_back(e);
  dead_.push_back(0);
  rank_.push_back(kNone);  // 与条目数同步（order_valid_ 已为 false）
  u32 idx = (u32)(entries_.size() - 1);
  by_lpath_.emplace(hash_ci(opath), idx);
  if (finalized_) {
    relock_append_overflow(idx);
    relock_link_parent(idx);
    order_valid_ = false;  // rank 未覆盖新条目
    ++pending_updates_;
    std::lock_guard<std::mutex> cg(cache_mtx_);
    cache_.valid = false;
  }
  return idx;
}

// 已持锁：新条目的 gram 写入溢出区（finalize 时并入扁平主结构）
void FileIndex::relock_append_overflow(u32 idx) {
  auto insert = [&](std::string_view s) {
    for (size_t j = 0; j + 1 < s.size(); ++j) {
      if (j > 0 && s[j] == s[j - 1] && s[j + 1] == s[j]) continue;
      auto& v = gram_overflow_[gram_key((unsigned char)s[j],
                                        (unsigned char)s[j + 1])];
      if (v.empty() || v.back() != idx) v.push_back(idx);  // 集合语义
    }
  };
  insert(lname(idx));
  if (entry(idx).pname_len) insert(pname(idx));
}

// 已持锁：把条目挂到父目录的子树列表（路径模式用）
void FileIndex::relock_link_parent(u32 idx) {
  std::string_view op = opath(idx);
  size_t p = op.find_last_of('/');
  if (p == std::string_view::npos || p == 0) return;  // 根层无父
  std::string_view parent = op.substr(0, p);
  auto range = by_lpath_.equal_range(hash_ci(parent));
  for (auto it = range.first; it != range.second; ++it) {
    if (!dead_[it->second] && path_eq_ci(opath(it->second), to_lower(std::string(parent)))) {
      dir_children_[it->second].push_back(idx);
      return;
    }
  }
}

const u32* FileIndex::gram_find(u32 key, size_t& n) const {
  auto it = std::lower_bound(gram_keys_.begin(), gram_keys_.end(), key);
  if (it == gram_keys_.end() || *it != key) { n = 0; return nullptr; }
  size_t i = (size_t)(it - gram_keys_.begin());
  size_t a = (size_t)gram_off_[i], b = (size_t)gram_off_[i + 1];
  n = b - a;
  return gram_data_.data() + a;
}

void FileIndex::finalize() {
  std::unique_lock<std::shared_mutex> g(mtx_);
  // 1) 扁平倒排重建（遍历全部存活条目，天然吸收溢出区）
  // 先按 (entry,gram) 集合语义发射到桶，再按桶大小扁平化——计数与写入
  // 天然一致（历史缺陷：count 按出现次数、emit 按集合去重，口径不一导致
  // 段尾残留 0 空洞，污染候选与升序假设）。
  std::unordered_map<u32, std::vector<u32>> buckets;
  buckets.reserve(1u << 16);
  for (u32 i = 0; i < entries_.size(); ++i) {
    if (dead_[i]) continue;
    auto emit = [&](std::string_view s) {
      for (size_t j = 0; j + 1 < s.size(); ++j) {
        if (j > 0 && s[j] == s[j - 1] && s[j + 1] == s[j])
          continue;  // 连续重复字节对（如 00000）只计一次
        u32 key = gram_key((unsigned char)s[j], (unsigned char)s[j + 1]);
        auto& b = buckets[key];
        if (b.empty() || b.back() != i) b.push_back(i);  // 集合语义去重
      }
    };
    emit(lname(i));
    if (entry(i).pname_len) emit(pname(i));
  }
  gram_keys_.clear();
  gram_keys_.reserve(buckets.size());
  for (const auto& kv : buckets) gram_keys_.push_back(kv.first);
  std::sort(gram_keys_.begin(), gram_keys_.end());
  gram_off_.assign(gram_keys_.size() + 1, 0);
  u64 total = 0;
  for (size_t i = 0; i < gram_keys_.size(); ++i) {
    total += buckets[gram_keys_[i]].size();
    gram_off_[i + 1] = total;
  }
  gram_data_.resize((size_t)total);
  {
    size_t pos = 0;
    for (u32 key : gram_keys_)
      for (u32 e : buckets[key]) gram_data_[pos++] = e;
  }
  buckets.clear();
  gram_overflow_.clear();

  // 2) rank（名序整数化）
  std::vector<u32> order;
  order.reserve(entries_.size());
  for (u32 i = 0; i < entries_.size(); ++i)
    if (!dead_[i]) order.push_back(i);
  auto by_name = [this](u32 a, u32 b) {
    auto na = lname(a), nb = lname(b);
    if (na != nb) return na < nb;
    return opath(a) < opath(b);
  };
  std::sort(order.begin(), order.end(), by_name);
  rank_.assign(entries_.size(), kNone);
  for (size_t pos = 0; pos < order.size(); ++pos) rank_[order[pos]] = (u32)pos;
  order_valid_ = true;

  // 3) 目录子树索引重建
  dir_children_.clear();
  for (u32 i = 0; i < entries_.size(); ++i)
    if (!dead_[i]) relock_link_parent(i);

  pending_updates_ = 0;
  finalized_ = true;
  std::lock_guard<std::mutex> cg(cache_mtx_);
  cache_.valid = false;
}

void FileIndex::clear() {
  std::unique_lock<std::shared_mutex> g(mtx_);
  std::lock_guard<std::mutex> cg(cache_mtx_);
  lname_arena_.clear(); opath_arena_.clear();
  pname_arena_.clear();
  entries_.clear(); dead_.clear(); by_lpath_.clear();
  gram_keys_.clear(); gram_off_.clear(); gram_data_.clear();
  gram_overflow_.clear(); dir_children_.clear(); rank_.clear();
  finalized_ = false; order_valid_ = false; pending_updates_ = 0;
  cache_.valid = false;
}

void FileIndex::swap(FileIndex& o) {
  if (this == &o) return;
  // std::lock 死锁避免：并发 A.swap(B)/B.swap(A) 时两把锁一次性获取
  std::unique_lock<std::shared_mutex> g1(mtx_, std::defer_lock);
  std::unique_lock<std::shared_mutex> g2(o.mtx_, std::defer_lock);
  std::lock(g1, g2);
  lname_arena_.swap(o.lname_arena_);
  opath_arena_.swap(o.opath_arena_);
  pname_arena_.swap(o.pname_arena_);
  entries_.swap(o.entries_);
  dead_.swap(o.dead_);
  by_lpath_.swap(o.by_lpath_);
  gram_keys_.swap(o.gram_keys_);
  gram_off_.swap(o.gram_off_);
  gram_data_.swap(o.gram_data_);
  gram_overflow_.swap(o.gram_overflow_);
  dir_children_.swap(o.dir_children_);
  rank_.swap(o.rank_);
  std::swap(finalized_, o.finalized_);
  std::swap(order_valid_, o.order_valid_);
  std::swap(pending_updates_, o.pending_updates_);
  std::swap(roots_, o.roots_);
  std::lock_guard<std::mutex> c1(cache_mtx_);
  std::lock_guard<std::mutex> c2(o.cache_mtx_);
  cache_.valid = false;
  o.cache_.valid = false;
}

size_t FileIndex::entry_count() const {
  std::shared_lock<std::shared_mutex> g(mtx_);
  return entries_.size();
}
size_t FileIndex::alive_count() const {
  std::shared_lock<std::shared_mutex> g(mtx_);
  size_t n = 0;
  for (u32 i = 0; i < entries_.size(); ++i) n += dead_[i] ? 0 : 1;
  return n;
}
bool FileIndex::finalized() const {
  std::shared_lock<std::shared_mutex> g(mtx_);
  return finalized_;
}

std::string_view FileIndex::lname(u32 i) const {
  const Entry& e = entries_[i];
  return {lname_arena_.data() + e.lname_off, e.lname_len};
}
std::string_view FileIndex::opath(u32 i) const {
  const Entry& e = entries_[i];
  return {opath_arena_.data() + e.opath_off, e.opath_len};
}
std::string_view FileIndex::pname(u32 i) const {
  const Entry& e = entries_[i];
  return {pname_arena_.data() + e.pname_off, e.pname_len};
}

u32 FileIndex::find_by_path(const std::string& opath) const {
  std::shared_lock<std::shared_mutex> g(mtx_);
  std::string lp = to_lower(opath);
  auto range = by_lpath_.equal_range(hash_ci(opath));
  for (auto it = range.first; it != range.second; ++it)
    if (!dead_[it->second] && path_eq_ci(this->opath(it->second), lp))
      return it->second;
  return kNone;
}

bool FileIndex::remove_by_path(const std::string& opath) {
  std::unique_lock<std::shared_mutex> g(mtx_);
  std::string lp = to_lower(opath);
  auto range = by_lpath_.equal_range(hash_ci(opath));
  for (auto it = range.first; it != range.second; ++it) {
    if (!dead_[it->second] && path_eq_ci(this->opath(it->second), lp)) {
      dead_[it->second] = 1;
      ++pending_updates_;
      std::lock_guard<std::mutex> cg(cache_mtx_);
      cache_.valid = false;
      return true;
    }
  }
  return false;
}

u32 FileIndex::upsert(const std::string& opath, u64 size, std::int64_t mtime,
                      EntryType type, const std::string& pname) {
  std::string lp = to_lower(opath);
  std::string ln = lp.substr(lp.find_last_of('/') + 1);
  // 标记旧条目 dead 与追加新条目在同一把写锁内完成（原两段式存在
  // TOCTOU：并发 upsert 同路径可产生重复存活条目/瞬时"消失"）
  std::unique_lock<std::shared_mutex> g(mtx_);
  auto range = by_lpath_.equal_range(hash_ci(opath));
  for (auto it = range.first; it != range.second; ++it)
    if (!dead_[it->second] && path_eq_ci(this->opath(it->second), lp))
      dead_[it->second] = 1;
  return relock_add(ln, opath, pname, size, mtime, type);
}

void FileIndex::snapshot_row(u32 i, u64& size, std::int64_t& mtime,
                             EntryType& type, std::string& opath) const {
  std::shared_lock<std::shared_mutex> g(mtx_);
  const Entry& e = entries_[i];
  size = e.size;
  mtime = e.mtime_sec;
  type = (EntryType)e.type;
  auto sv = this->opath(i);
  opath.assign(sv.data(), sv.size());
}

bool FileIndex::alive(u32 i) const { return !dead_[i]; }

size_t FileIndex::pending_updates() const { return pending_updates_; }

size_t FileIndex::remove_by_path_prefix(const std::string& opath_prefix) {
  std::unique_lock<std::shared_mutex> g(mtx_);
  std::string p = to_lower(opath_prefix);
  if (p.empty()) return 0;
  if (p.back() != '/') p += '/';
  size_t n = 0;
  for (u32 i = 0; i < entries_.size(); ++i) {
    if (dead_[i]) continue;
    if (ci_starts_with(opath(i), p)) {
      dead_[i] = 1;
      ++n;
    }
  }
  if (n) {
    pending_updates_ += n;
    std::lock_guard<std::mutex> cg(cache_mtx_);
    cache_.valid = false;
  }
  return n;
}

void FileIndex::set_roots(const std::vector<std::string>& roots) {
  std::unique_lock<std::shared_mutex> g(mtx_);
  roots_.clear();
  roots_.reserve(roots.size());
  for (const auto& r : roots) roots_.push_back(to_lower(r));
}

std::vector<std::string> FileIndex::all_dir_paths() const {
  std::shared_lock<std::shared_mutex> g(mtx_);
  std::vector<std::string> out;
  for (u32 i = 0; i < entries_.size(); ++i)
    if (!dead_[i] && entries_[i].type == EntryType::Dir) {
      const Entry& e = entries_[i];
      out.emplace_back(opath_arena_.data() + e.opath_off, e.opath_len);
    }
  return out;
}

IndexStats FileIndex::stats() const {
  std::shared_lock<std::shared_mutex> g(mtx_);
  IndexStats s;
  s.entry_count = entries_.size();
  s.lname_bytes = lname_arena_.size();
  s.opath_bytes = opath_arena_.size();
  s.pname_bytes = pname_arena_.size();
  s.gram_keys = gram_keys_.size();
  s.gram_postings = gram_data_.size();
  for (const auto& kv : gram_overflow_) s.gram_overflow += kv.second.size();
  for (u32 i = 0; i < entries_.size(); ++i) s.alive_count += dead_[i] ? 0 : 1;
  s.order_valid = order_valid_;
  return s;
}

bool FileIndex::pass_filters(u32 i, const Query& q) const {
  const Filters& f = q.filt;
  const Entry& e = entries_[i];
  if (f.type_filter) {
    bool is_file = e.type == EntryType::File;
    if (f.type_filter == 1 && !is_file) return false;
    if (f.type_filter == 2 && is_file) return false;
  }
  if (f.has_ext) {
    std::string_view n = lname(i);
    size_t dot = n.find_last_of('.');
    std::string_view ext = dot == std::string_view::npos ? std::string_view{}
                                                         : n.substr(dot + 1);
    bool ok = false;
    for (const auto& want : f.exts)
      if (ext == want) { ok = true; break; }
    if (!ok) return false;
  }
  if (f.size_cmp) {
    switch (f.size_cmp) {
      case 1: if (!(e.size > f.size_bytes)) return false; break;
      case 2: if (!(e.size >= f.size_bytes)) return false; break;
      case 3: if (!(e.size < f.size_bytes)) return false; break;
      case 4: if (!(e.size <= f.size_bytes)) return false; break;
      default: if (e.size != f.size_bytes) return false; break;
    }
  }
  if (f.has_dm && !(e.mtime_sec >= f.dm_from && e.mtime_sec < f.dm_to))
    return false;
  if (f.has_path_sub && !contains_ci(opath(i), f.path_sub, false)) return false;
  return true;
}

// 单词条完整匹配（含大小写/全字/路径模式语义；供 Term 求值与缓存过滤）
bool FileIndex::term_match(u32 i, const std::string& term, const Query& q) const {
  std::string low = to_lower(term);
  bool hit;
  if (q.flags.match_path) {
    hit = contains_ci(opath(i), low, q.flags.whole_word);
  } else {
    hit = contains(lname(i), low, q.flags.whole_word);
    if (!hit && !q.flags.case_sensitive && !q.flags.whole_word &&
        entry(i).pname_len)
      hit = contains(pname(i), low, false);
  }
  if (hit && q.flags.case_sensitive) {
    std::string_view orig =
        q.flags.match_path ? opath(i) : last_component(opath(i));
    hit = contains(orig, term, q.flags.whole_word);
  }
  return hit;
}

void FileIndex::collect_subtree(u32 dir, std::vector<u32>& out) const {
  std::vector<u32> stack{dir};
  while (!stack.empty()) {
    u32 d = stack.back();
    stack.pop_back();
    out.push_back(d);
    auto it = dir_children_.find(d);
    if (it == dir_children_.end()) continue;
    for (u32 c : it->second) {
      if (dead_[c]) continue;
      if (entries_[c].type == EntryType::Dir) stack.push_back(c);
      else out.push_back(c);
    }
  }
}

void FileIndex::term_candidates(const Query& q, size_t term_idx,
                                std::vector<u32>& out) const {
  const std::string& term = q.terms[term_idx];
  const std::string low = to_lower(term);

  // 大小写敏感：在原始文本上验证（lower 候选必为超集）
  auto cs_filter = [&](std::vector<u32>& v) {
    if (!q.flags.case_sensitive) return;
    std::vector<u32> kept;
    kept.reserve(v.size());
    for (u32 i : v) {
      std::string_view orig =
          q.flags.match_path ? opath(i) : last_component(opath(i));
      if (contains(orig, term, q.flags.whole_word)) kept.push_back(i);
    }
    v = std::move(kept);
  };

  // ---- 路径模式 ----
  if (q.flags.match_path) {
    // 跨段词（含 '/'，如 assets/icons）与全字模式：线性扫保全语义
    if (low.find('/') != std::string::npos || q.flags.whole_word) {
      for (u32 i = 0; i < entries_.size(); ++i)
        if (!dead_[i] && contains_ci(opath(i), low, q.flags.whole_word))
          out.push_back(i);
      cs_filter(out);
      return;
    }
    // 词命中扫描根前缀（根之上的目录不入索引）：全集命中
    for (const auto& root : roots_) {
      if (contains(root, low, false)) {
        for (u32 i = 0; i < entries_.size(); ++i)
          if (!dead_[i]) out.push_back(i);
        cs_filter(out);
        return;
      }
    }
    // 单段词：自身名包含 ∪ 命中目录的整棵子树
    for (u32 i = 0; i < entries_.size(); ++i) {  // 目录名线性（目录 ~ n/10）
      if (dead_[i] || entries_[i].type != EntryType::Dir) continue;
      if (contains(lname(i), low, false)) collect_subtree(i, out);
    }
    for (u32 i = 0; i < entries_.size(); ++i)  // 自身名包含
      if (!dead_[i] && contains(lname(i), low, false)) out.push_back(i);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    cs_filter(out);
    return;
  }

  // ---- 名模式：gram 驱动 ----
  if (low.size() < 2) {
    for (u32 i = 0; i < entries_.size(); ++i)
      if (!dead_[i] && term_match(i, term, q)) out.push_back(i);
    return;
  }
  // 选最小驱动 gram（扁平区 + 溢出区），任一 gram 全缺席即无匹配
  const u32* best = nullptr;
  size_t best_n = 0;
  const std::vector<u32>* best_ov = nullptr;
  for (size_t j = 0; j + 1 < low.size(); ++j) {
    u32 key = gram_key((unsigned char)low[j], (unsigned char)low[j + 1]);
    size_t n = 0;
    const u32* span = gram_find(key, n);
    const std::vector<u32>* ov = nullptr;
    auto oit = gram_overflow_.find(key);
    if (oit != gram_overflow_.end()) ov = &oit->second;
    size_t total = n + (ov ? ov->size() : 0);
    if (total == 0) return;
    if (!best || total < best_n + (best_ov ? best_ov->size() : 0)) {
      best = span;
      best_n = n;
      best_ov = ov;
    }
  }
  // 扁平区与溢出区各自升序且互不相交 → 归并遍历（同键重复字节对已去重，
  // 但跨 lname/pname 的非相邻重复仍可能相邻出现，兜底跳过）
  size_t a = 0, b = 0;
  while ((best && a < best_n) || (best_ov && b < best_ov->size())) {
    u32 i;
    if (best && a < best_n &&
        (!best_ov || b >= best_ov->size() || best[a] <= (*best_ov)[b]))
      i = best[a++];
    else
      i = (*best_ov)[b++];
    if (!out.empty() && out.back() == i) continue;
    if (!dead_[i] && term_match(i, term, q)) out.push_back(i);
  }
}

void FileIndex::eval_node(const QNode& n, const Query& q,
                          std::vector<u32>& out) const {
  out.clear();
  if (n.kind == QNode::Term) {
    term_candidates(q, (size_t)n.term, out);
    return;
  }
  if (n.kind == QNode::Not) {  // 顶层 NOT：谓词扫描全集取反
    for (u32 i = 0; i < entries_.size(); ++i)
      if (pred_node(n, q, i)) out.push_back(i);
    return;
  }
  if (n.kind == QNode::Or) {
    bool has_not = false;
    for (const auto& k : n.kids)
      if (k.kind == QNode::Not) has_not = true;
    if (has_not) {  // OR 含负词：谓词回退（少见）
      for (u32 i = 0; i < entries_.size(); ++i)
        if (!dead_[i] && pred_node(n, q, i)) out.push_back(i);
      return;
    }
    for (const auto& k : n.kids) {
      std::vector<u32> v;
      eval_node(k, q, v);
      out.insert(out.end(), v.begin(), v.end());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return;
  }
  // And：正词相交、负词做差
  bool first = true;
  for (const auto& k : n.kids) {
    if (k.kind == QNode::Not) {
      std::vector<u32> neg;
      eval_node(k.kids[0], q, neg);
      if (first) {
        for (u32 i = 0; i < entries_.size(); ++i)
          if (!dead_[i]) out.push_back(i);
        first = false;
      }
      std::vector<u32> kept;
      kept.reserve(out.size());
      size_t a = 0, b = 0;
      while (a < out.size() && b < neg.size()) {
        if (out[a] == neg[b]) { ++a; ++b; }
        else if (out[a] < neg[b]) kept.push_back(out[a++]);
        else ++b;
      }
      while (a < out.size()) kept.push_back(out[a++]);
      out = std::move(kept);
      continue;
    }
    std::vector<u32> v;
    eval_node(k, q, v);
    if (first) {
      out = std::move(v);
      first = false;
      continue;
    }
    std::vector<u32> nxt;
    nxt.reserve(out.size());
    size_t a = 0, b = 0;
    while (a < out.size() && b < v.size()) {
      if (out[a] == v[b]) { nxt.push_back(out[a]); ++a; ++b; }
      else if (out[a] < v[b]) ++a;
      else ++b;
    }
    out = std::move(nxt);
  }
  if (first) out.clear();
}

// 谓词回退（含 NOT 的 OR / 顶层 NOT 用）
bool FileIndex::pred_node(const QNode& n, const Query& q, u32 i) const {
  switch (n.kind) {
    case QNode::Term:
      return !dead_[i] && term_match(i, q.terms[(size_t)n.term], q);
    case QNode::And:
      for (const auto& k : n.kids)
        if (!pred_node(k, q, i)) return false;
      return !dead_[i];
    case QNode::Or:
      for (const auto& k : n.kids)
        if (pred_node(k, q, i)) return !dead_[i];
      return false;
    case QNode::Not:
      return !dead_[i] && !pred_node(n.kids[0], q, i);
  }
  return false;
}

void FileIndex::sort_results(std::vector<u32>& v) const {
  if (order_valid_) {
    std::sort(v.begin(), v.end(),
              [this](u32 a, u32 b) { return rank_[a] < rank_[b]; });
    return;
  }
  auto cmp = [this](u32 a, u32 b) {
    auto na = lname(a), nb = lname(b);
    if (na != nb) return na < nb;
    return opath(a) < opath(b);
  };
  std::sort(v.begin(), v.end(), cmp);
}

void FileIndex::cache_reset() const {
  std::lock_guard<std::mutex> g(cache_mtx_);
  cache_.valid = false;
}

void FileIndex::search(const Query& q, std::vector<u32>& out) const {
  out.clear();
  std::shared_lock<std::shared_mutex> g(mtx_);
  if (!finalized_) return;

#ifdef KYSEARCH_HAVE_RE2
  if (q.flags.regex) {
    // 正则使用原始输入（含 (?i) 等内联标志），不被布尔词法拆散
    RE2 re(q.raw.empty() ? std::string(q.terms.empty() ? "" : q.terms[0])
                         : q.raw);
    if (!re.ok()) return;
    // 正则匹配原始大小写文本（string_view 直传 re2，零拷贝）
    for (u32 i = 0; i < entries_.size(); ++i) {
      if (dead_[i]) continue;
      bool hit = q.flags.match_path
                     ? RE2::PartialMatch(opath(i), re)
                     : RE2::PartialMatch(last_component(opath(i)), re);
      if (hit && pass_filters(i, q)) out.push_back(i);
    }
    sort_results(out);
    return;
  }
#endif

  // ---- 增量缓存：末词延伸时在上次结果上过滤（连续打字热路径） ----
  std::lock_guard<std::mutex> cguard(cache_mtx_);
  if (q.flat_and && cache_.valid && q.terms.size() == cache_.terms.size() &&
      flags_eq(q.flags, cache_.flags) && filters_eq(q.filt, cache_.filt)) {
    bool prefix_ok = true;
    for (size_t i = 0; i + 1 < q.terms.size(); ++i)
      if (q.terms[i] != cache_.terms[i]) { prefix_ok = false; break; }
    const std::string& old_last = cache_.terms.back();
    const std::string& new_last = q.terms.back();
    if (prefix_ok && !old_last.empty() && new_last.size() > old_last.size() &&
        new_last.compare(0, old_last.size(), old_last) == 0) {
      std::vector<u32> refined;
      refined.reserve(cache_.results.size());
      for (u32 i : cache_.results)
        if (term_match(i, new_last, q)) refined.push_back(i);
      cache_.terms.back() = new_last;
      cache_.results = refined;
      out = std::move(refined);
      return;
    }
  }

  std::vector<u32> base;
  if (q.terms.empty()) {
    for (u32 i = 0; i < entries_.size(); ++i)
      if (!dead_[i]) base.push_back(i);
  } else {
    eval_node(q.root, q, base);
  }
  std::vector<u32> filtered;
  filtered.reserve(base.size());
  for (u32 i : base)
    if (pass_filters(i, q)) filtered.push_back(i);
  sort_results(filtered);

  if (q.flat_and) {
    cache_.valid = true;
    cache_.terms = q.terms;
    cache_.flags = q.flags;
    cache_.filt = q.filt;
    cache_.results = filtered;
  }
  out = std::move(filtered);
}

// ---- 快照 v3：含扁平倒排 + rank；原子落盘 ----

namespace {

struct SnapHeader {
  char magic[4] = {'K', 'Y', 'S', '4'};
  u32 version = 4;
  u32 entry_count = 0;
  u64 lname_size = 0, opath_size = 0, pname_size = 0;
  u64 entry_bytes = 0, dead_bytes = 0;
  u64 gram_key_count = 0, gram_off_bytes = 0, gram_data_count = 0;
  u64 rank_bytes = 0;
  u32 overflow_lists = 0;
  u64 overflow_postings = 0;
  u32 order_valid_flag = 0;  // rank 是否可信（upsert 后未重建则不可信）
  u64 roots_bytes = 0;       // 扫描根串（小写，\n 分隔）
};
static_assert(std::is_trivially_copyable_v<SnapHeader>, "");
static_assert(std::is_trivially_copyable_v<Entry>, "");

}  // namespace

bool FileIndex::save(const std::string& path) const {
  std::unique_lock<std::shared_mutex> g(mtx_);
  std::string tmp = path + ".tmp";
  std::FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = true;
  SnapHeader h;
  h.entry_count = (u32)entries_.size();
  h.lname_size = lname_arena_.size();
  h.opath_size = opath_arena_.size();
  h.pname_size = pname_arena_.size();
  h.entry_bytes = entries_.size() * sizeof(Entry);
  h.dead_bytes = dead_.size();
  h.gram_key_count = gram_keys_.size();
  h.gram_off_bytes = gram_off_.size() * sizeof(u64);
  h.gram_data_count = gram_data_.size();
  h.order_valid_flag = order_valid_ ? 1 : 0;
  h.rank_bytes = order_valid_ ? rank_.size() * sizeof(u32) : 0;
  std::string roots_blob;
  for (const auto& r : roots_) {
    roots_blob += r;
    roots_blob.push_back('\n');
  }
  h.roots_bytes = roots_blob.size();
  for (const auto& kv : gram_overflow_) {
    if (!kv.second.empty()) {
      h.overflow_lists++;
      h.overflow_postings += kv.second.size();
    }
  }
  ok &= std::fwrite(&h, sizeof h, 1, f) == 1;
  auto wr = [&](const void* p, size_t n) { ok &= std::fwrite(p, 1, n, f) == n; };
  wr(lname_arena_.data(), h.lname_size);
  wr(opath_arena_.data(), h.opath_size);
  wr(pname_arena_.data(), h.pname_size);
  wr(entries_.data(), h.entry_bytes);
  wr(dead_.data(), h.dead_bytes);
  wr(gram_keys_.data(), h.gram_key_count * sizeof(u32));
  wr(gram_off_.data(), h.gram_off_bytes);
  wr(gram_data_.data(), h.gram_data_count * sizeof(u32));
  if (h.rank_bytes) wr(rank_.data(), h.rank_bytes);
  for (const auto& kv : gram_overflow_) {
    if (kv.second.empty()) continue;
    u32 key = kv.first;
    u32 cnt = (u32)kv.second.size();
    wr(&key, sizeof key);
    wr(&cnt, sizeof cnt);
    wr(kv.second.data(), (size_t)cnt * sizeof(u32));
  }
  wr(roots_blob.data(), h.roots_bytes);
  ok &= std::fflush(f) == 0;
  if (ok) ok &= ::fsync(::fileno(f)) == 0;  // 崩溃安全
  std::fclose(f);
  if (!ok) {
    ::unlink(tmp.c_str());
    return false;
  }
  return ::rename(tmp.c_str(), path.c_str()) == 0;
}

bool FileIndex::load(const std::string& path, FileIndex& out) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  SnapHeader h;
  bool ok = std::fread(&h, sizeof h, 1, f) == 1;
  if (!ok || h.magic[0] != 'K' || h.magic[1] != 'Y' || h.magic[2] != 'S' ||
      h.version != 4) {
    std::fclose(f);
    return false;
  }
  FileIndex idx;
  // 快照属于不可信输入（损坏/恶意构造均可导致堆溢出）：头部字段先做
  // 一致性校验——总量（防乘法回绕，用字节级检查）不得超出文件实际
  // 大小，entry 字节数必须与条目数严格匹配
  {
    auto checked_add = [](u64& acc, u64 n) {
      return n <= UINT64_MAX - acc && (acc += n, true);
    };
    u64 declared = 16;
    const bool sums_ok =
        checked_add(declared, h.lname_size) &&
        checked_add(declared, h.opath_size) &&
        checked_add(declared, h.pname_size) &&
        checked_add(declared, h.entry_bytes) && checked_add(declared, h.dead_bytes) &&
        // gram_key_count*4 换成字节数上限检查，防 u64 回绕
        checked_add(declared, (h.gram_key_count >> 62) ? UINT64_MAX : h.gram_key_count * 4) &&
        checked_add(declared, h.gram_off_bytes) &&
        checked_add(declared, (h.gram_data_count >> 62) ? UINT64_MAX : h.gram_data_count * 4) &&
        checked_add(declared, h.rank_bytes) && checked_add(declared, h.roots_bytes);
    std::fseek(f, 0, SEEK_END);
    const long fsize = std::ftell(f);
    std::fseek(f, (long)sizeof h, SEEK_SET);
    if (!sums_ok || fsize < 0 || declared > (u64)fsize ||
        h.entry_bytes != (u64)h.entry_count * sizeof(Entry) ||
        (h.gram_key_count >> 62) || (h.gram_data_count >> 62)) {
      std::fclose(f);
      return false;
    }
  }
  auto rd = [](std::vector<char>& v, u64 n, std::FILE* f) {
    v.resize((size_t)n);
    return n == 0 || std::fread(v.data(), 1, (size_t)n, f) == n;
  };
  ok &= rd(idx.lname_arena_, h.lname_size, f);
  ok &= rd(idx.opath_arena_, h.opath_size, f);
  ok &= rd(idx.pname_arena_, h.pname_size, f);
  idx.entries_.resize(h.entry_count);
  ok &= h.entry_bytes == 0 ||
        std::fread(idx.entries_.data(), 1, (size_t)h.entry_bytes, f) ==
            (size_t)h.entry_bytes;
  idx.dead_.resize((size_t)h.dead_bytes);
  ok &= h.dead_bytes == 0 ||
        std::fread(idx.dead_.data(), 1, (size_t)h.dead_bytes, f) ==
            (size_t)h.dead_bytes;
  idx.gram_keys_.resize((size_t)h.gram_key_count);
  ok &= h.gram_key_count == 0 ||
        std::fread(idx.gram_keys_.data(), 4, (size_t)h.gram_key_count, f) ==
            (size_t)h.gram_key_count;
  idx.gram_off_.resize((size_t)h.gram_off_bytes / sizeof(u64));
  ok &= h.gram_off_bytes == 0 ||
        std::fread(idx.gram_off_.data(), sizeof(u64),
                   (size_t)h.gram_off_bytes / sizeof(u64), f) ==
            (size_t)h.gram_off_bytes / sizeof(u64);
  idx.gram_data_.resize((size_t)h.gram_data_count);
  ok &= h.gram_data_count == 0 ||
        std::fread(idx.gram_data_.data(), 4, (size_t)h.gram_data_count, f) ==
            (size_t)h.gram_data_count;
  idx.rank_.resize(h.rank_bytes ? (size_t)(h.rank_bytes / sizeof(u32)) : 0);
  ok &= h.rank_bytes == 0 ||
        std::fread(idx.rank_.data(), 4, (size_t)(h.rank_bytes / sizeof(u32)),
                   f) == (size_t)(h.rank_bytes / sizeof(u32));
  for (u32 i = 0; ok && i < h.overflow_lists; ++i) {
    u32 key = 0, cnt = 0;
    ok &= std::fread(&key, 4, 1, f) == 1;
    ok &= std::fread(&cnt, 4, 1, f) == 1;
    if (!ok || cnt == 0) { ok = cnt != 0; break; }
    // cnt 上界：剩余文件字节数（防伪造计数的 GB 级分配 DoS）
    {
      long cur = std::ftell(f);
      std::fseek(f, 0, SEEK_END);
      long fend = std::ftell(f);
      std::fseek(f, cur, SEEK_SET);
      if (fend < 0 || cur < 0 || (u64)(fend - cur) < (u64)cnt * 4) {
        ok = false;
        break;
      }
    }
    std::vector<u32> v((size_t)cnt);
    ok &= std::fread(v.data(), 4, (size_t)cnt, f) == (size_t)cnt;
    idx.gram_overflow_.emplace(key, std::move(v));
  }
  if (ok && h.roots_bytes) {
    std::string blob((size_t)h.roots_bytes, '\0');
    ok = std::fread(blob.data(), 1, blob.size(), f) == blob.size();
    if (ok) {
      size_t start = 0;
      while (start < blob.size()) {
        size_t nl = blob.find('\n', start);
        std::string one = blob.substr(start, nl - start);
        if (!one.empty()) idx.roots_.push_back(one);
        if (nl == std::string::npos) break;
        start = nl + 1;
      }
    }
  }
  std::fclose(f);
  if (!ok) return false;
  // 结构一致性校验：倒排区间/条目引用/arena 边界（损坏快照拒绝加载）
  auto validate = [&]() -> bool {
    const bool both_empty = idx.gram_keys_.empty() && idx.gram_off_.empty();
    if (!both_empty && idx.gram_off_.size() != idx.gram_keys_.size() + 1)
      return false;
    for (size_t k = 0; k < idx.gram_off_.size(); ++k) {
      if (idx.gram_off_[k] > (u64)idx.gram_data_.size()) return false;
      if (k && idx.gram_off_[k] < idx.gram_off_[k - 1]) return false;
    }
    for (u32 e : idx.gram_data_)
      if (e >= idx.entries_.size()) return false;
    for (const auto& kv : idx.gram_overflow_) {
      if (kv.first > 0xFFFF) return false;  // gram key 是 16 位
      for (u32 e : kv.second)
        if (e >= idx.entries_.size()) return false;
    }
    for (u32 i = 0; i < idx.entries_.size(); ++i) {
      const Entry& e = idx.entries_[i];
      if ((u64)e.lname_off + e.lname_len > idx.lname_arena_.size() ||
          (u64)e.opath_off + e.opath_len > idx.opath_arena_.size() ||
          (u64)e.pname_off + e.pname_len > idx.pname_arena_.size())
        return false;
    }
    return true;
  };
  if (!validate()) return false;
  if (idx.dead_.size() != idx.entries_.size())
    idx.dead_.assign(idx.entries_.size(), 0);
  if (idx.rank_.size() != idx.entries_.size())
    idx.rank_.assign(idx.entries_.size(), kNone);

  // 仅重建哈希表与目录树（O(n)，无 gram 重建、无排序）
  {
    std::unique_lock<std::shared_mutex> g2(idx.mtx_);
    for (u32 i = 0; i < idx.entries_.size(); ++i) {
      if (idx.dead_[i]) continue;
      idx.by_lpath_.emplace(hash_ci(idx.opath(i)), i);
    }
    for (u32 i = 0; i < idx.entries_.size(); ++i)
      if (!idx.dead_[i]) idx.relock_link_parent(i);
    idx.finalized_ = true;
    idx.order_valid_ = h.order_valid_flag == 1 &&
                       idx.rank_.size() == idx.entries_.size();
    if (!idx.order_valid_) idx.rank_.assign(idx.entries_.size(), kNone);
    idx.pending_updates_ = 0;
  }
  out.swap(idx);
  return true;
}

}  // namespace kysearch
