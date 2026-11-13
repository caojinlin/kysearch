// 中文拼音键生成（M3）
// 文件名 -> "全拼|首字母|多音组合" 变体串（\x1f 分隔，控制字符不可输入，
// 变体间不会产生跨界误匹配）。仅含 CJK 的名字生成，否则返回空串。
// 查询侧由 FileIndex 的 pname arena + 2-gram 倒排承接（见 index.cpp）。
#pragma once

#include <string>

namespace kysearch {

// 例：手机001.jpg -> "shouji001.jpg\x1fsj001.jpg"（多音字会追加更多变体）
std::string pinyin_key(const std::string& utf8_name);

}  // namespace kysearch
