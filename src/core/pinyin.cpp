#include "core/pinyin.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "core/index.h"  // u32
#include "core/pinyin_data.h"

namespace kysearch {

namespace {

bool lookup(u32 cp, const pinyin_data::Entry*& out) {
  size_t lo = 0, hi = pinyin_data::kSize;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (pinyin_data::kTable[mid].cp < cp) lo = mid + 1;
    else hi = mid;
  }
  if (lo < pinyin_data::kSize && pinyin_data::kTable[lo].cp == cp) {
    out = &pinyin_data::kTable[lo];
    return true;
  }
  return false;
}

inline char ascii_lower_c(char c) {
  return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

// 模糊音（标准五对）：z↔zh c↔ch s↔sh n↔l f↔h —— 返回替换后的替代读音
std::string fuzzy_alt(const std::string& s) {
  struct Rule { const char* from; const char* to; };
  static const Rule kRules[] = {
      {"zh", "z"}, {"z", "zh"}, {"ch", "c"}, {"c", "ch"},
      {"sh", "s"}, {"s", "sh"}, {"n", "l"},  {"l", "n"},
      {"f", "h"},  {"h", "f"},
  };
  for (const auto& r : kRules) {
    size_t n = std::strlen(r.from);
    if (s.size() > n && s.compare(0, n, r.from) == 0)
      return std::string(r.to) + s.substr(n);
  }
  return s;
}

// 解析全部 UTF-8 码点（容错：非法序列按单字节处理）
struct Cp {
  u32 cp;
  size_t off, len;  // 原始字节切片
};

}  // namespace

std::string pinyin_key(const std::string& name) {
  if (name.empty()) return {};

  // ASCII 快速预检：纯 ASCII 名无拼音键，免解码直接返回
  bool has_nonascii = false;
  for (char c : name)
    if ((unsigned char)c >= 0x80) { has_nonascii = true; break; }
  if (!has_nonascii) return {};

  // 解码
  std::vector<Cp> cps;
  cps.reserve(name.size());
  for (size_t i = 0; i < name.size();) {
    unsigned char b = (unsigned char)name[i];
    size_t len = 1;
    u32 cp = b;
    if (b >= 0xF0) len = 4;
    else if (b >= 0xE0) len = 3;
    else if (b >= 0xC0) len = 2;
    if (len > 1 && i + len <= name.size()) {
      cp = b & (0xFF >> (len + 1));
      for (size_t k = 1; k < len; ++k)
        cp = (cp << 6) | ((unsigned char)name[i + k] & 0x3F);
    } else {
      len = 1;
      cp = b;
    }
    cps.push_back(Cp{cp, i, len});
    i += len;
  }

  // 读音表：CJK 查表，其他原样（小写化 ASCII）
  struct Item {
    std::vector<std::string> readings;  // 默认在前；非 CJK 为空 + keep 字节
    std::string raw_lower;              // 非 CJK 时的原始切片（ASCII 小写）
    bool is_cjk = false;
  };
  std::vector<Item> items;
  bool has_cjk = false;
  for (const auto& c : cps) {
    const pinyin_data::Entry* e = nullptr;
    if (c.cp >= 0x4E00 && c.cp <= 0x9FA5 && lookup(c.cp, e)) {
      has_cjk = true;
      Item it;
      it.is_cjk = true;
      std::string all = e->all;
      size_t start = 0;
      while (start <= all.size()) {
        size_t comma = all.find(',', start);
        std::string one = all.substr(start, comma == std::string::npos
                                                ? std::string::npos
                                                : comma - start);
        if (!one.empty()) it.readings.push_back(one);
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
      it.readings[0] = e->def;  // 表内默认读音本就第一，双保险
      items.push_back(std::move(it));
    } else {
      Item it;
      for (size_t k = 0; k < c.len; ++k)
        it.raw_lower.push_back(ascii_lower_c(name[c.off + k]));
      items.push_back(std::move(it));
    }
  }
  if (!has_cjk) return {};

  // 变体展开：全拼（上限 16，含模糊音）与首字母（上限 16，含模糊首字母）
  constexpr char SEP = '\x1f';
  std::vector<std::string> variants;
  auto expand = [&](bool initials, size_t cap) {
    std::vector<std::string> cur{std::string()};
    for (const auto& it : items) {
      if (!it.is_cjk) {
        for (auto& s : cur) s += it.raw_lower;
        continue;
      }
      // 候选读音：全部读音 + 模糊音替代（去重，默认在前）
      std::vector<std::string> readings = it.readings;
      for (const auto& r : it.readings) {
        std::string alt = fuzzy_alt(r);
        if (std::find(readings.begin(), readings.end(), alt) == readings.end())
          readings.push_back(std::move(alt));
      }
      std::vector<std::string> nxt;
      nxt.reserve(cur.size() * readings.size());
      for (const auto& base : cur) {
        for (const auto& r : readings) {
          if (nxt.size() >= cap) break;
          nxt.push_back(base + (initials ? std::string(1, r[0]) : r));
        }
      }
      if (nxt.empty()) {  // 超限兜底：全部退回默认读音
        for (const auto& base : cur)
          nxt.push_back(base + (initials
                                    ? std::string(1, it.readings[0][0])
                                    : it.readings[0]));
      }
      cur = std::move(nxt);
    }
    for (auto& s : cur) variants.push_back(std::move(s));
  };
  expand(false, 16);
  expand(true, 16);

  // 去重合并
  std::sort(variants.begin(), variants.end());
  variants.erase(std::unique(variants.begin(), variants.end()), variants.end());
  std::string out;
  for (size_t i = 0; i < variants.size(); ++i) {
    if (i) out.push_back(SEP);
    out += variants[i];
  }
  return out;
}

}  // namespace kysearch
