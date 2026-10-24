#include "ui/ImageView.h"
#include <QPainter>
#include <cmath>

namespace ivs {
ImageView::ImageView(QWidget *parent) : QWidget(parent) {
    setMinimumSize(300, 230);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAutoFillBackground(false);
}
void ImageView::setResult(const InspectionResult &result) {
    m_result = result;
    update();
}
void ImageView::clear() {
    m_result = {};
    update();
}

void ImageView::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(QStringLiteral("#121b27")));
    const QRectF area = QRectF(rect()).adjusted(12, 12, -12, -44);
    if (m_result.frame.isNull()) {
        painter.setPen(QPen(QColor(QStringLiteral("#34465c")), 1));
        const QPointF center = area.center() - QPointF(0, 22);
        const QRectF camera(center.x() - 30, center.y() - 23, 60, 46);
        painter.drawRoundedRect(camera, 7, 7);
        painter.drawEllipse(center, 11, 11);
        painter.drawLine(QPointF(camera.left() + 12, camera.top()),
                         QPointF(camera.left() + 18, camera.top() - 8));
        painter.drawLine(QPointF(camera.left() + 18, camera.top() - 8),
                         QPointF(camera.right() - 18, camera.top() - 8));
        painter.setPen(QColor(QStringLiteral("#a0afbf")));
        painter.drawText(QRectF(area.left(), center.y() + 40, area.width(), 24), Qt::AlignCenter,
                         tr("等待检测图像"));
        painter.setPen(QColor(QStringLiteral("#65758a")));
        painter.drawText(QRectF(area.left(), center.y() + 67, area.width(), 20), Qt::AlignCenter,
                         tr("启动检测后显示图像、轮廓与测量中心"));
    } else {
        QSizeF fitted = m_result.frame.size();
        fitted.scale(area.size(), Qt::KeepAspectRatio);
        const QRectF destination(area.center().x() - fitted.width() / 2,
                                 area.center().y() - fitted.height() / 2, fitted.width(), fitted.height());
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(destination, m_result.frame);
        const QRectF bounds = m_result.objectBounds.normalized();
        const bool validBounds = std::isfinite(bounds.x()) && std::isfinite(bounds.y()) &&
                                 std::isfinite(bounds.width()) && std::isfinite(bounds.height()) &&
                                 !bounds.isEmpty();
        if (validBounds) {
            const QRectF clipped = bounds.intersected(QRectF(m_result.frame.rect()));
            if (!clipped.isEmpty()) {
                const qreal scaleX = destination.width() / m_result.frame.width();
                const qreal scaleY = destination.height() / m_result.frame.height();
                const QRectF overlay(destination.left() + clipped.left() * scaleX,
                                     destination.top() + clipped.top() * scaleY, clipped.width() * scaleX,
                                     clipped.height() * scaleY);
                const QColor color = m_result.record.outcome == QLatin1String("OK")
                                         ? QColor(QStringLiteral("#35e3a5"))
                                         : QColor(QStringLiteral("#ff7777"));
                painter.save();
                painter.setClipRect(destination);
                painter.setPen(QPen(color, 2));
                painter.setBrush(Qt::NoBrush);
                painter.drawRect(overlay);
                painter.drawEllipse(overlay);
                const QPointF center = overlay.center();
                painter.drawLine(center - QPointF(12, 0), center + QPointF(12, 0));
                painter.drawLine(center - QPointF(0, 12), center + QPointF(0, 12));
                painter.restore();
            }
        }
    }
    const QRectF footer(0, height() - 34, width(), 34);
    painter.fillRect(footer, QColor(QStringLiteral("#1b2838")));
    painter.setPen(QColor(QStringLiteral("#9caec1")));
    if (m_result.frame.isNull()) {
        painter.drawText(footer.adjusted(12, 0, -12, 0), Qt::AlignVCenter | Qt::AlignLeft,
                         tr("无图像 · 等待运行"));
    } else {
        QString outcome = m_result.record.outcome;
        if (outcome.isEmpty())
            outcome = tr("待判定");
        const QColor outcomeColor = outcome == QLatin1String("OK") ? QColor(QStringLiteral("#53dfad"))
                                                                   : QColor(QStringLiteral("#ff8c8c"));
        painter.setPen(outcomeColor);
        painter.drawText(footer.adjusted(12, 0, -12, 0), Qt::AlignVCenter | Qt::AlignLeft,
                         outcome + QStringLiteral("  ·  ") +
                             tr("直径 %1 mm").arg(m_result.record.diameterMm, 0, 'f', 3));
        painter.setPen(QColor(QStringLiteral("#9caec1")));
        painter.drawText(footer.adjusted(12, 0, -12, 0), Qt::AlignVCenter | Qt::AlignRight,
                         QStringLiteral("%1 × %2").arg(m_result.frame.width()).arg(m_result.frame.height()));
    }
}
} // namespace ivs
