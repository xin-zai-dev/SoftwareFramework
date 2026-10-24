#pragma once
#include "domain/Types.h"
#include <QAbstractTableModel>

namespace ivs {
class RecordTableModel : public QAbstractTableModel {
    Q_OBJECT
  public:
    explicit RecordTableModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void setRecords(const QList<DetectionRecord> &records);
    void prependRecord(const DetectionRecord &record);
    QList<DetectionRecord> records() const {
        return m_records;
    }

  private:
    QList<DetectionRecord> m_records;
};
} // namespace ivs
