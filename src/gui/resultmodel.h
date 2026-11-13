// 结果表模型
// 本地模式：只存 entry 下标（u32），百万行结果仅几 MB，QTableView 虚拟化渲染。
// 远程模式：守护进程返回的行数据（路径等字符串）。
#pragma once

#include <QAbstractTableModel>
#include <QString>
#include <vector>

#include "core/index.h"

class ResultModel : public QAbstractTableModel {
  Q_OBJECT
 public:
  enum Col { ColName = 0, ColPath, ColSize, ColMtime, ColType, ColCount };

  struct RemoteRow {
    QString type, name, path, size, mtime;
    qint64 sizeBytes = 0;  // 原始字节数（排序用；size 仅供显示）
  };

  explicit ResultModel(QObject* parent = nullptr);

  void setIndex(const kysearch::FileIndex* idx);
  void setResults(std::vector<kysearch::u32> rows);      // 本地模式
  void setRemoteResults(std::vector<RemoteRow> rows);    // 远程模式
  bool remote() const { return remote_; }
  QString rowPath(int row) const;  // 两种模式统一的路径取值

  // 列排序（点击表头；本地/远程两种数据源各自比较器）
  void applySort(int col, Qt::SortOrder order);
  const std::vector<kysearch::u32>& localRows() const { return rows_; }
  const std::vector<RemoteRow>& remoteRows() const { return remote_rows_; }

  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& idx, int role) const override;
  QVariant headerData(int sec, Qt::Orientation o, int role) const override;

 private:
  const kysearch::FileIndex* idx_ = nullptr;
  std::vector<kysearch::u32> rows_;
  std::vector<RemoteRow> remote_rows_;
  bool remote_ = false;
};
