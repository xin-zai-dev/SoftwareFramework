#include "ui/ChartWidget.h"
#include "domain/Project.h"
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr int maximumRecords = 1000;
QString numberLabel(long double value) {
    const double finite =
        static_cast<double>(qBound(-static_cast<long double>(std::numeric_limits<double>::max()), value,
                                   static_cast<long double>(std::numeric_limits<double>::max())));
    return QString::number(
        finite, (std::abs(finite) >= 100000 || (finite != 0 && std::abs(finite) < 0.001)) ? 'g' : 'f',
        std::abs(finite) < 10 ? 3 : 2);
}
void expandRange(long double &minimum, long double &maximum) {
    const long double span = maximum - minimum;
    const long double padding = span > 0 ? span * 0.08L : std::max(1.0L, std::abs(minimum) * 0.05L);
    minimum -= padding;
    maximum += padding;
}
} // namespace

namespace ivs {
ChartWidget::ChartWidget(Kind kind, QWidget *parent) : QWidget(parent), m_kind(kind) {
    setMinimumSize(290, 210);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAutoFillBackground(false);
}

void ChartWidget::setField(const QString &field) {
    m_field = field;
    update();
}
void ChartWidget::setRecords(const QList<DetectionRecord> &records) {
    m_records = records.mid(qMax(qsizetype(0), records.size() - maximumRecords));
    update();
}
void ChartWidget::appendRecord(const DetectionRecord &record) {
    m_records.append(record);
    if (m_records.size() > maximumRecords)
        m_records.removeFirst();
    update();
}
void ChartWidget::clear() {
    m_records.clear();
    update();
}

void ChartWidget::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QColor(QStringLiteral("#dfe6ed")));
    painter.setBrush(Qt::white);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    QFont heading = font();
    heading.setBold(true);
    painter.setFont(heading);
    painter.setPen(QColor(QStringLiteral("#26384a")));
    painter.drawText(QRectF(16, 9, width() - 32, 22), Qt::AlignLeft | Qt::AlignVCenter,
                     (m_kind == Kind::Trend ? tr("趋势 · ") : tr("散点 · ")) + fieldTitle(m_field));
    QFont labelFont = font();
    labelFont.setPointSizeF(qMax(8.0, labelFont.pointSizeF() - 1));
    painter.setFont(labelFont);
    const QRectF plot(70, 43, qMax(1, width() - 94), qMax(1, height() - 98));

    struct Sample {
        long double x;
        long double y;
        bool ng;
    };
    QList<Sample> samples;
    long double minX = std::numeric_limits<long double>::infinity();
    long double maxX = -minX;
    long double minY = minX;
    long double maxY = -minX;
    for (qsizetype i = 0; i < m_records.size(); ++i) {
        const auto &record = m_records.at(i);
        const double x = m_kind == Kind::Trend ? double(i + 1) : record.offsetXmm;
        const double y = recordField(record, m_field);
        if (!std::isfinite(x) || !std::isfinite(y))
            continue;
        samples.append(
            {x, y, record.outcome == QLatin1String("NG") || record.outcome == QLatin1String("ERROR")});
        minX = std::min(minX, static_cast<long double>(x));
        maxX = std::max(maxX, static_cast<long double>(x));
        minY = std::min(minY, static_cast<long double>(y));
        maxY = std::max(maxY, static_cast<long double>(y));
    }
    if (samples.isEmpty()) {
        minX = 0;
        maxX = 1;
        minY = 0;
        maxY = 1;
    } else {
        if (m_kind == Kind::Trend && minX != maxX) {
            minX = 1;
            maxX = qMax(qsizetype(2), m_records.size());
        } else
            expandRange(minX, maxX);
        expandRange(minY, maxY);
    }
    for (int tick = 0; tick <= 4; ++tick) {
        const qreal ratio = tick / 4.0;
        const qreal x = plot.left() + ratio * plot.width();
        const qreal y = plot.bottom() - ratio * plot.height();
        painter.setPen(QPen(QColor(QStringLiteral("#e8edf3")), 1, Qt::DashLine));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.setPen(QColor(QStringLiteral("#718095")));
        painter.drawText(QRectF(4, y - 10, plot.left() - 12, 20), Qt::AlignRight | Qt::AlignVCenter,
                         numberLabel(minY + (maxY - minY) * ratio));
        const long double tickX = minX + (maxX - minX) * ratio;
        painter.drawText(QRectF(x - 40, plot.bottom() + 5, 80, 18), Qt::AlignHCenter | Qt::AlignVCenter,
                         m_kind == Kind::Trend ? QString::number(static_cast<qint64>(std::round(tickX)))
                                               : numberLabel(tickX));
    }
    painter.setPen(QColor(QStringLiteral("#a8b5c3")));
    painter.drawLine(plot.bottomLeft(), plot.bottomRight());
    painter.drawLine(plot.bottomLeft(), plot.topLeft());
    painter.setPen(QColor(QStringLiteral("#718095")));
    painter.drawText(QRectF(plot.left(), height() - 27, plot.width(), 18), Qt::AlignCenter,
                     m_kind == Kind::Trend ? tr("最近检测序号") : tr("X 偏移 (mm)"));
    if (samples.isEmpty()) {
        painter.setPen(QColor(QStringLiteral("#8996a6")));
        painter.drawText(plot, Qt::AlignCenter, tr("等待检测数据"));
        return;
    }
    const auto pointFor = [&](const Sample &sample) {
        return QPointF(plot.left() + qreal((sample.x - minX) / (maxX - minX)) * plot.width(),
                       plot.bottom() - qreal((sample.y - minY) / (maxY - minY)) * plot.height());
    };
    painter.save();
    painter.setClipRect(plot.adjusted(-5, -5, 5, 5));
    const QColor blue(QStringLiteral("#2879df"));
    if (m_kind == Kind::Trend && samples.size() > 1) {
        QPainterPath path;
        path.moveTo(pointFor(samples.first()));
        for (qsizetype i = 1; i < samples.size(); ++i)
            path.lineTo(pointFor(samples.at(i)));
        painter.setPen(QPen(blue, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }
    for (const auto &sample : samples) {
        const QColor color = sample.ng ? QColor(QStringLiteral("#df5d55")) : blue;
        if (samples.size() > 150)
            painter.setPen(Qt::NoPen);
        else
            painter.setPen(QPen(Qt::white, 1));
        painter.setBrush(color);
        const qreal radius = samples.size() > 150 ? 2.2 : 4.0;
        painter.drawEllipse(pointFor(sample), radius, radius);
    }
    painter.restore();
}
} // namespace ivs
