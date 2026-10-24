#include "domain/ImageAnalysis.h"
#include <cmath>
#ifdef IVS_WITH_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#endif
#ifdef IVS_WITH_HALCON
#include <HalconCpp.h>
#endif

namespace ivs {
bool analyzeFrame(const QImage &frame, double pixelsPerMm, ImageMeasurement *out, QString *error) {
    const auto fail = [&](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    if (!out || frame.isNull())
        return fail("图像为空");
    if (!std::isfinite(pixelsPerMm) || pixelsPerMm <= 0)
        return fail("像素标定比例无效");
    if (qint64(frame.width()) * frame.height() > 16777216)
        return fail("图像超过16777216像素限制");
    const QImage gray = frame.convertToFormat(QImage::Format_Grayscale8);
    QRect bounds;
#ifdef IVS_WITH_OPENCV
    try {
        cv::Mat input(gray.height(), gray.width(), CV_8UC1, const_cast<uchar *>(gray.constBits()),
                      gray.bytesPerLine());
        cv::Mat binary;
        cv::threshold(input, binary, 127, 255, cv::THRESH_BINARY);
        const auto box = cv::boundingRect(binary);
        if (box.width > 0 && box.height > 0)
            bounds = QRect(box.x, box.y, box.width, box.height);
    } catch (const cv::Exception &e) {
        return fail(QString::fromUtf8(e.what()));
    }
#else
    int minX = gray.width(), minY = gray.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < gray.height(); ++y) {
        const auto *line = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x)
            if (line[x] > 127) {
                minX = qMin(minX, x);
                maxX = qMax(maxX, x);
                minY = qMin(minY, y);
                maxY = qMax(maxY, y);
            }
    }
    if (maxX >= minX && maxY >= minY)
        bounds = QRect(QPoint(minX, minY), QPoint(maxX, maxY));
#endif
    if (bounds.width() < 2 || bounds.height() < 2)
        return fail("未检测到亮色工件，请检查曝光、背景和阈值");
    ImageMeasurement m;
    m.bounds = bounds;
    m.diameterPixels = (bounds.width() + bounds.height()) / 2.0;
    m.diameterMm = m.diameterPixels / pixelsPerMm;
    m.offsetXmm = (bounds.x() + bounds.width() / 2.0 - gray.width() / 2.0) / pixelsPerMm;
    m.offsetYmm = (bounds.y() + bounds.height() / 2.0 - gray.height() / 2.0) / pixelsPerMm;
    *out = m;
    if (error)
        error->clear();
    return true;
}
QString algorithmBackend() {
#ifdef IVS_WITH_OPENCV
    return QStringLiteral("OpenCV · 阈值与几何测量");
#else
    return QStringLiteral("Qt C++ · 阈值与几何测量");
#endif
}
QString halconStatus() {
#ifdef IVS_WITH_HALCON
    return QStringLiteral("HALCON SDK 已链接（自定义算法扩展入口，需有效运行授权）");
#else
    return QStringLiteral("HALCON 未启用（可选 SDK，不影响基础运行）");
#endif
}
} // namespace ivs
