#include "ui/RecordTableModel.h"
#include <QBrush>
#include <QColor>
namespace ivs {
RecordTableModel::RecordTableModel(QObject *parent) : QAbstractTableModel(parent) {}
int RecordTableModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_records.size();
}
int RecordTableModel::columnCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : 9;
}
QVariant RecordTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole)
        return {};
    if (orientation == Qt::Vertical)
        return section + 1;
    static const QStringList names{"记录 ID",   "检测时间",  "工件编号", "判定", "直径 mm",
                                   "X 偏移 mm", "Y 偏移 mm", "耗时 ms",  "图像"};
    return section >= 0 && section < names.size() ? names[section] : QVariant{};
}
QVariant RecordTableModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_records.size())
        return {};
    const auto &r = m_records[index.row()];
    if (role == Qt::TextAlignmentRole)
        return int(index.column() == 2 ? Qt::AlignLeft | Qt::AlignVCenter : Qt::AlignCenter);
    if (role == Qt::ForegroundRole && index.column() == 3)
        return QBrush(QColor(r.outcome == "OK" ? "#118769" : r.outcome == "NG" ? "#d34b5d" : "#bb810a"));
    if (role == Qt::ToolTipRole)
        return QString("运行: %1\n配方: %2\n%3\n%4").arg(r.runId, r.recipe, r.message, r.imagePath);
    if (role != Qt::DisplayRole)
        return {};
    switch (index.column()) {
    case 0:
        return r.id;
    case 1:
        return r.timestamp.toLocalTime().toString("MM-dd HH:mm:ss.zzz");
    case 2:
        return r.workpieceId;
    case 3:
        return r.outcome;
    case 4:
        return QString::number(r.diameterMm, 'f', 3);
    case 5:
        return QString::number(r.offsetXmm, 'f', 3);
    case 6:
        return QString::number(r.offsetYmm, 'f', 3);
    case 7:
        return QString::number(r.durationMs, 'f', 1);
    case 8:
        return r.imagePath.isEmpty() ? QStringLiteral("—") : QStringLiteral("查看");
    default:
        return {};
    }
}
void RecordTableModel::setRecords(const QList<DetectionRecord> &records) {
    beginResetModel();
    m_records = records;
    endResetModel();
}
void RecordTableModel::prependRecord(const DetectionRecord &record) {
    beginInsertRows({}, 0, 0);
    m_records.prepend(record);
    endInsertRows();
    if (m_records.size() > 200) {
        beginRemoveRows({}, 200, m_records.size() - 1);
        m_records = m_records.mid(0, 200);
        endRemoveRows();
    }
}
} // namespace ivs
