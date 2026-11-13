// kysearch 并行目录扫描器（M1）
// 多线程 BFS：工作线程从目录队列取活，子目录回填队列，文件批量合并进索引。
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "core/index.h"

namespace kysearch {

struct ScanOptions {
  std::vector<std::string> roots;
  // 按名字排除的目录（.git / node_modules / .cache 等）
  std::vector<std::string> exclude_names = {".git", "node_modules",
                                            "__pycache__", ".cache"};
  unsigned threads = 0;  // 0 = 自动（核数，上限 16）
  // 周期性进度回调（在工作线程上调用，接收方需自行保证线程安全）
  std::function<void(size_t files, size_t dirs)> progress;
  // 可选：文件名 -> 拼音键（全拼+首字母）；由上层注入（M3 拼音搜索）
  std::function<std::string(const std::string& name)> pinyin_fn;
  // 取消标志（GUI 退出时置位；扫描周期性检查，剩余队列弃置）
  const std::atomic<bool>* cancel = nullptr;
};

class Scanner {
 public:
  explicit Scanner(FileIndex& idx) : idx_(idx) {}
  void run(const ScanOptions& opt);

 private:
  FileIndex& idx_;
};

}  // namespace kysearch
