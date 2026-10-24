#pragma once
#include "domain/Types.h"
#include <QWidget>

namespace ivs {
class ChartWidget : public QWidget {
    Q_OBJECT
  public:
    enum class Kind { Trend, Scatter };
    explicit ChartWidget(Kind kind, QWidget *parent = nullptr);
    void setField(const QString &field);
    void setRecords(const QList<DetectionRecord> &records);
    void appendRecord(const DetectionRecord &record);
    void clear();

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    Kind m_kind;
    QString m_field = QStringLiteral("diameterMm");
    QList<DetectionRecord> m_records;
};
} // namespace ivs
