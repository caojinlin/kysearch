#include <algorithm>
#include "gui/resultmodel.h"

#include <QDateTime>

#include <cstdio>

namespace {

QString human_size(kysearch::u64 b) {
  char buf[32];
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  int u = 0;
  double v = (double)b;
  while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
  if (u == 0) std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b);
  else std::snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
  return QString::fromUtf8(buf);
}

QString fromSv(std::string_view sv) {
  return QString::fromUtf8(sv.data(), (int)sv.size());
}

}  // namespace

ResultModel::ResultModel(QObject* parent) : QAbstractTableModel(parent) {}

void ResultModel::setIndex(const kysearch::FileIndex* idx) {
  beginResetModel();
  idx_ = idx;
  remote_ = false;
  rows_.clear();
  endResetModel();
}

void ResultModel::setResults(std::vector<kysearch::u32> rows) {
  beginResetModel();
  remote_ = false;
  rows_ = std::move(rows);
  endResetModel();
}

void ResultModel::setRemoteResults(std::vector<RemoteRow> rows) {
  beginResetModel();
  remote_ = true;
  remote_rows_ = std::move(rows);
  endResetModel();
}

QString ResultModel::rowPath(int row) const {
  // 统一边界校验：模型重置/空结果期间的历史调用不再越界
  if (remote_) {
    if (row < 0 || row >= (int)remote_rows_.size()) return {};
    return remote_rows_[(size_t)row].path;
  }
  if (!idx_ || row < 0 || row >= (int)rows_.size()) return {};
  return fromSv(idx_->opath(rows_[(size_t)row]));
}

void ResultModel::applySort(int col, Qt::SortOrder order) {
  bool asc = order == Qt::AscendingOrder;
  // 降序一律用「交换参数的同一个严格 less」实现：!r && name!= 的写法在
  // 键相等时双向返回 true，违反严格弱序，std::sort 行为未定义
  if (remote_) {
    auto key_less = [col](const RemoteRow& a, const RemoteRow& b) {
      switch (col) {
        case ColSize: return a.sizeBytes < b.sizeBytes;  // 字节数而非格式化串
        case ColMtime: return a.mtime < b.mtime;
        case ColType: return a.type < b.type;
        case ColPath: return a.path < b.path;
        default: return a.name < b.name;
      }
    };
    beginResetModel();
    std::sort(remote_rows_.begin(), remote_rows_.end(),
              [&key_less, asc](const RemoteRow& a, const RemoteRow& b) {
                return asc ? key_less(a, b) : key_less(b, a);
              });
    endResetModel();
    return;
  }
  if (!idx_) return;
  auto key_less = [this, col](kysearch::u32 a, kysearch::u32 b) {
    const kysearch::Entry& ea = idx_->entry(a);
    const kysearch::Entry& eb = idx_->entry(b);
    switch (col) {
      case ColSize: return ea.size < eb.size;
      case ColMtime: return ea.mtime_sec < eb.mtime_sec;
      case ColType: return ea.type < eb.type;
      case ColPath: return idx_->opath(a) < idx_->opath(b);
      default: return idx_->lname(a) < idx_->lname(b);
    }
  };
  beginResetModel();
  std::sort(rows_.begin(), rows_.end(),
            [&key_less, asc](kysearch::u32 a, kysearch::u32 b) {
              return asc ? key_less(a, b) : key_less(b, a);
            });
  endResetModel();
}

int ResultModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0
         : remote_ ? (int)remote_rows_.size() : (int)rows_.size();
}
int ResultModel::columnCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : ColCount;
}

QVariant ResultModel::data(const QModelIndex& idx, int role) const {
  if (!idx.isValid() || idx.row() >= rowCount()) return {};
  const int c = idx.column();

  if (remote_) {
    const RemoteRow& r = remote_rows_[(size_t)idx.row()];
    if (role == Qt::DisplayRole) {
      switch (c) {
        case ColName: return r.name;
        case ColPath: return r.path.section('/', 0, -2);
        case ColSize: return r.type == "D" ? QStringLiteral("—") : r.size;
        case ColMtime: return r.mtime;
        case ColType: return r.type == "D" ? tr("文件夹") : tr("文件");
      }
    } else if (role == Qt::TextAlignmentRole &&
               (c == ColSize || c == ColMtime)) {
      return int(Qt::AlignRight | Qt::AlignVCenter);
    }
    return {};
  }

  if (!idx_) return {};
  const auto row = rows_[(size_t)idx.row()];
  const auto e = idx_->entry(row);

  if (role == Qt::DisplayRole) {
    switch (c) {
      case ColName: {
        auto op = idx_->opath(row);
        size_t p = op.find_last_of('/');
        return fromSv(p == std::string_view::npos ? op : op.substr(p + 1));
      }
      case ColPath: {
        auto op = idx_->opath(row);
        size_t p = op.find_last_of('/');
        if (p == std::string_view::npos || p == 0) return fromSv(op.substr(0, 1));
        return fromSv(op.substr(0, p));
      }
      case ColSize:
        return e.type == kysearch::EntryType::Dir
                   ? QStringLiteral("—")
                   : human_size(e.size);
      case ColMtime:
        return QDateTime::fromSecsSinceEpoch((qint64)e.mtime_sec)
            .toString(QStringLiteral("yyyy-MM-dd hh:mm"));
      case ColType:
        return e.type == kysearch::EntryType::Dir ? tr("文件夹") : tr("文件");
    }
  } else if (role == Qt::TextAlignmentRole && (c == ColSize || c == ColMtime)) {
    return int(Qt::AlignRight | Qt::AlignVCenter);
  }
  return {};
}

QVariant ResultModel::headerData(int sec, Qt::Orientation o, int role) const {
  if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
  static const char* heads[] = {QT_TR_NOOP("名称"), QT_TR_NOOP("路径"),
                                QT_TR_NOOP("大小"), QT_TR_NOOP("修改时间"),
                                QT_TR_NOOP("类型")};
  if (sec < 0 || sec >= ColCount) return {};
  return tr(heads[sec]);
}
