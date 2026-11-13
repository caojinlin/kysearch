// 全文内容索引（M4）：SQLite FTS5 + trigram 分词
//   - 子串语义：中英文关键词一律按"包含"匹配（trigram 需要 >=3 字节，
//     更短的词自动退化为 LIKE）
//   - 范围控制：仅索引白名单纯文本扩展名、单文件 <= 上限（默认 2MB）
//   - 数据库与主索引快照同目录：content.db
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "core/index.h"  // u32/u64/Query/FileIndex

struct sqlite3;

namespace kysearch {

class FileIndex;
struct Query;

class TextIndex {
 public:
  static constexpr u64 kDefaultMaxBytes = 2 * 1024 * 1024;

  TextIndex() = default;
  ~TextIndex();
  TextIndex(const TextIndex&) = delete;
  TextIndex& operator=(const TextIndex&) = delete;

  // 打开/建库；FTS5 trigram 不可用时返回 false（内容搜索优雅降级）
  bool open(const std::string& db_path);
  bool ok() const { return db_ != nullptr; }

  // 是否属于内容索引范围（扩展名白名单 + 可选大小上限）
  static bool want_file(const std::string& path,
                        u64 max_bytes = kDefaultMaxBytes,
                        u64 file_size = kDefaultMaxBytes);

  bool index_file(const std::string& path);  // 读取并入库（覆盖旧内容）
  void remove_file(const std::string& path);
  size_t count() const;

  // 全部词 AND；返回命中路径（bm25 排序，上限 limit）
  std::vector<std::string> search(const std::vector<std::string>& terms,
                                  size_t limit = 1000) const;

 private:
  sqlite3* db_ = nullptr;
  bool trigram_ = false;  // SQLite >= 3.34 才有；false = LIKE 降级模式
  mutable std::mutex mtx_;
};

// 查询含 content: 词时，用内容命中路径过滤主索引结果（无内容词原样返回）
void apply_content_filter(const TextIndex& t, const Query& q,
                          const FileIndex& idx, std::vector<u32>& res);

}  // namespace kysearch
