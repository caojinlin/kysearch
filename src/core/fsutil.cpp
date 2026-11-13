#include "core/fsutil.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace kysearch {

namespace {

std::string url_escape(const std::string& s) {
  // freedesktop 规范：Path 字段 URI 转义（空格等）
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  char buf[3];
  for (unsigned char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '-' || c == '_')
      out += (char)c;
    else {
      buf[0] = '%';
      buf[1] = hex[c >> 4];
      buf[2] = hex[c & 0xF];
      out.append(buf, 3);
    }
  }
  return out;
}

}  // namespace

bool trash_move(const std::string& path, std::string* err,
                const std::string& trash_base) {
  std::string base = trash_base;
  if (base.empty()) {
    // freedesktop：$XDG_DATA_HOME/Trash，未设时回退 ~/.local/share/Trash
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg)
      base = std::string(xdg) + "/Trash";
    else {
      const char* home = std::getenv("HOME");
      if (!home) {
        if (err) *err = "HOME 未设置";
        return false;
      }
      base = std::string(home) + "/.local/share/Trash";
    }
  }
  std::error_code ec;
  fs::path abs = fs::absolute(fs::path(path), ec);
  if (ec || !fs::exists(abs, ec)) {
    if (err) *err = "文件不存在: " + path;
    return false;
  }
  fs::path files = fs::path(base) / "files";
  fs::path info = fs::path(base) / "info";
  fs::create_directories(files, ec);
  fs::create_directories(info, ec);

  // 命名冲突：name、name.2、name.3 …（查询出错按"已占用"处理：
  // 误判为可用会让 POSIX rename 覆盖回收站里的既有文件）
  std::string stem = abs.filename().string();
  fs::path dest;
  for (int i = 1;; ++i) {
    std::string cand = i == 1 ? stem : stem + "." + std::to_string(i);
    fs::path try_dest = files / cand;
    if (!fs::exists(try_dest, ec) && !ec) {
      dest = try_dest;
      break;
    }
    ec.clear();
    if (i > 10000) {  // 防病态冲突无限循环
      if (err) *err = "回收站命名冲突无法消解";
      return false;
    }
  }

  // .trashinfo 必须先成功落盘（否则文件不可还原），失败即中止
  std::time_t t = std::time(nullptr);
  std::tm tmv{};
  localtime_r(&t, &tmv);
  char when[32] = {0};
  if (std::strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", &tmv) == 0) {
    if (err) *err = "时间格式化失败";
    return false;
  }
  fs::path info_file = info / (dest.filename().string() + ".trashinfo");
  {
    std::ofstream f(info_file, std::ios::trunc);
    if (!f) {
      if (err) *err = "无法写入 " + info_file.string() + "（回收站只读/磁盘满？）";
      return false;
    }
    f << "[Trash Info]\nPath=file://" << url_escape(abs.string())
      << "\nDeletionDate=" << when << "\n";
    f.close();
    if (f.fail()) {
      if (err) *err = "写入 .trashinfo 失败（磁盘满？）";
      std::error_code e2;
      fs::remove(info_file, e2);
      return false;
    }
  }

  std::error_code re;
  fs::rename(abs, dest, re);
  if (re) {
    // 仅跨文件系统才降级为复制+删除；其他错误（权限/不存在）如实上报
    if (re != std::errc::cross_device_link) {
      if (err) *err = "移动失败: " + re.message();
      std::error_code e2;
      fs::remove(info_file, e2);  // 回滚孤儿 .trashinfo
      return false;
    }
    std::error_code c1;
    if (fs::is_directory(dest, c1) || fs::is_directory(abs, c1))
      fs::copy(abs, dest, fs::copy_options::recursive, c1);
    else
      fs::copy_file(abs, dest, c1);
    if (c1) {
      if (err) *err = "复制失败: " + c1.message();
      std::error_code e2;
      fs::remove_all(dest, e2);   // 目录回滚须整树清理
      fs::remove(info_file, e2);  // 一并清掉孤儿 .trashinfo
      return false;
    }
    std::error_code c2;
    fs::remove_all(abs, c2);
    if (c2) {
      if (err) *err = "删除源失败: " + c2.message();
      return false;
    }
  }
  return true;
}

}  // namespace kysearch
