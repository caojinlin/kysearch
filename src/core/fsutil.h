// 文件系统工具（POSIX）：回收站删除（freedesktop Trash 规范）
#pragma once

#include <string>

namespace kysearch {

// 把文件/目录移入回收站（~/.local/share/Trash，或指定 trash_base 用于测试）。
// 成功返回 true；err（可选）回填失败原因。跨设备自动退化为复制+删除。
bool trash_move(const std::string& path, std::string* err = nullptr,
                const std::string& trash_base = "");

}  // namespace kysearch
