// kysearch 核心索引（v4：布尔查询 / rank 排序 / 增量缓存 / 扁平倒排 / 路径子树）
//   - arena x3：小写文件名 / 原始全路径 / 拼音键（v4 去除小写全路径冗余，
//     大小写不敏感路径匹配改为零分配逐字符比较，内存 -30%）
//   - 锁：shared_mutex 读共享写独占（查询并发），增量缓存独立小锁
//   - 扁平 2-gram 倒排（key 升序 + 偏移 + 连续 posting）+ 增量溢出区：
//     finalize 时压缩进主结构，upsert 即时写溢出区保证实时可搜
//   - rank 预计算：finalize 生成名序 rank，查询排序退化为整数比较
//   - 增量缓存：连续打字（末词延伸）时在上次结果上过滤，免重查重排
//   - 路径模式：自身名 gram 命中 ∪ 命中目录的整棵子树（O(目录数+结果)）
//   - 布尔：a|b、!a、( ) 混合；全正词 AND 走快速路径
#pragma once

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kysearch {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

enum class EntryType : std::uint8_t { File = 0, Dir = 1 };
constexpr u32 kNone = 0xFFFFFFFFu;

#pragma pack(push, 1)
struct Entry {
  u32 lname_off = 0, lname_len = 0;  // 小写文件名 -> lname arena
  u32 opath_off = 0, opath_len = 0;  // 原始全路径 -> opath arena（显示/大小写验证）
  u32 pname_off = 0, pname_len = 0;  // 拼音键 -> pname arena（可空）
  u64 size = 0;                      // 字节数（目录为 0）
  std::int64_t mtime_sec = 0;        // 修改时间（epoch 秒）
  EntryType type = EntryType::File;
};
#pragma pack(pop)
static_assert(sizeof(Entry) == 41, "Entry 布局变化需同步快照版本号");

struct QueryFlags {
  bool case_sensitive = false;  // 区分大小写
  bool whole_word = false;      // 全字匹配（边界不是 [A-Za-z0-9_]）
  bool match_path = false;      // 按全路径搜索（默认只搜文件名）
  bool regex = false;           // 按正则搜索（RE2；忽略布尔结构）
};

// Everything 风格过滤器
struct Filters {
  bool has_ext = false;
  std::vector<std::string> exts;
  int size_cmp = 0;                     // 0 无 1:> 2:>= 3:< 4:<= 5:=
  u64 size_bytes = 0;
  bool has_dm = false;
  std::int64_t dm_from = 0, dm_to = 0;
  int type_filter = 0;                  // 0 无 1:file 2:dir
  bool has_path_sub = false;
  std::string path_sub;
  bool empty() const {
    return !has_ext && size_cmp == 0 && !has_dm && type_filter == 0 &&
           !has_path_sub;
  }
};

// ---- 布尔语法树 ----
struct QNode {
  enum Kind : std::uint8_t { Term, And, Or, Not };
  Kind kind = And;
  int term = -1;               // Term：Query::terms 下标
  std::vector<QNode> kids;     // And/Or 子节点；Not 取 kids[0]
};

struct Query {
  std::string raw;                         // 原始输入（正则模式使用）
  std::vector<std::string> terms;          // 叶子词（原样）
  std::vector<std::string> content_terms;  // content:词（全文层消费）
  QueryFlags flags;
  Filters filt;
  QNode root;           // 无词条时为空 And
  bool flat_and = false;  // 全正词 AND（快速路径 + 增量缓存适用）
};

// 解析：a b（AND）|（OR）!（NOT）()；过滤器：ext: size:>10mb dm:today|7d|
// 2024-01-01|A..B type:file|dir path:xxx content:词；正则模式忽略布尔。
Query parse_query(const std::string& raw, QueryFlags flags);

struct IndexStats {
  size_t entry_count = 0, alive_count = 0;
  size_t lname_bytes = 0, opath_bytes = 0, pname_bytes = 0;
  size_t gram_keys = 0, gram_postings = 0, gram_overflow = 0;
  bool order_valid = false;
};

class FileIndex {
 public:
  // 多线程安全：内部加锁（扫描并发写 / 与查询并发共存）。
  // 注意：entry()/lname()/opath()/pname()/alive() 为热路径轻量访问器，
  // 仅在读多写少且调用方即刻消费返回值时使用；跨锁持有引用/string_view
  // 在并发写下会悬垂。需要稳定数据请用 snapshot_row（值拷贝）。
  u32 add(std::string_view lname, std::string_view opath,
          std::string_view pname, u64 size, std::int64_t mtime, EntryType type);

  // 单次调用在共享锁内取回整行数据（守护进程回包用它避免下标悬垂）
  void snapshot_row(u32 i, u64& size, std::int64_t& mtime, EntryType& type,
                    std::string& opath) const;

  void finalize();  // 重建扁平倒排(并吸收溢出) + rank + 目录子树索引
  bool finalized() const;
  void clear();
  void swap(FileIndex& o);

  size_t entry_count() const;
  size_t alive_count() const;
  bool alive(u32 i) const;
  const Entry& entry(u32 i) const { return entries_[i]; }
  std::string_view lname(u32 i) const;
  std::string_view opath(u32 i) const;
  std::string_view pname(u32 i) const;

  // ---- 增量维护 ----
  u32 upsert(const std::string& opath, u64 size, std::int64_t mtime,
             EntryType type, const std::string& pname = "");
  bool remove_by_path(const std::string& opath);
  u32 find_by_path(const std::string& opath) const;
  size_t remove_by_path_prefix(const std::string& opath_prefix);
  std::vector<std::string> all_dir_paths() const;
  // 登记扫描根（小写化；路径模式需覆盖根前缀之上的路径成分）
  void set_roots(const std::vector<std::string>& roots);
  const std::vector<std::string>& roots() const { return roots_; }
  // 自上次 finalize 后的增量条数（监控据此节流重建）
  size_t pending_updates() const;

  // 执行查询；out 为存活且命中的 entry 下标，按（文件名, 路径）升序
  void search(const Query& q, std::vector<u32>& out) const;
  void cache_reset() const;  // 基准/外部修改后清增量缓存

  IndexStats stats() const;

  // 原子落盘：path.tmp + fsync + rename（崩溃安全）
  bool save(const std::string& path) const;
  // 快照 v3：持久化倒排与 rank，加载免重建（仅重建哈希表/目录树）
  static bool load(const std::string& path, FileIndex& out);

 private:
  bool term_match(u32 i, const std::string& low, const Query& q) const;
  void term_candidates(const Query& q, size_t term_idx,
                       std::vector<u32>& out) const;
  void eval_node(const QNode& n, const Query& q, std::vector<u32>& out) const;
  bool pred_node(const QNode& n, const Query& q, u32 i) const;
  bool pass_filters(u32 i, const Query& q) const;
  void sort_results(std::vector<u32>& v) const;
  void relock_append_overflow(u32 idx);   // 已持锁：追加 gram 到溢出区
  void relock_link_parent(u32 idx);       // 已持锁：挂到父目录子树
  u32 relock_add(std::string_view lname, std::string_view opath,
                 std::string_view pname, u64 size, std::int64_t mtime,
                 EntryType type);  // 已持锁：add 的内核（upsert 单锁用）
  void collect_subtree(u32 dir, std::vector<u32>& out) const;
  const u32* gram_find(u32 key, size_t& n) const;  // 扁平区二分
  static u64 hash_ci(std::string_view opath);

  std::vector<char> lname_arena_, opath_arena_, pname_arena_;
  std::vector<Entry> entries_;
  std::vector<std::uint8_t> dead_;
  std::unordered_multimap<u64, u32> by_lpath_;
  // 扁平倒排
  std::vector<u32> gram_keys_;               // 升序
  std::vector<u64> gram_off_;                // keys+1
  std::vector<u32> gram_data_;
  std::unordered_map<u32, std::vector<u32>> gram_overflow_;
  // 路径子树：目录 entry idx -> 直接子 entry idx（含文件与目录）
  std::unordered_map<u32, std::vector<u32>> dir_children_;
  std::vector<u32> rank_;                    // entry idx -> 名序
  std::vector<std::string> roots_;           // 扫描根（小写，快照持久化）
  bool finalized_ = false;
  bool order_valid_ = false;                 // rank 是否反映追加条目
  size_t pending_updates_ = 0;               // finalize 后的增量计数
  mutable std::shared_mutex mtx_;            // 读共享 / 写独占

  // 增量结果缓存（连续打字：末词延伸时在上次结果上过滤）
  struct Cache {
    bool valid = false;
    std::vector<std::string> terms;
    QueryFlags flags;
    Filters filt;
    std::vector<u32> results;
  };
  mutable Cache cache_;
  mutable std::mutex cache_mtx_;
};

}  // namespace kysearch
