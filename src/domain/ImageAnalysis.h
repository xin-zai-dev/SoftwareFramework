#pragma once
#include <QImage>
#include <QRect>
#include <QString>

namespace ivs {
struct ImageMeasurement {
    QRect bounds;
    double diameterPixels = 0;
    double diameterMm = 0;
    double offsetXmm = 0;
    double offsetYmm = 0;
};
bool analyzeFrame(const QImage &frame, double pixelsPerMm, ImageMeasurement *measurement,
                  QString *error = nullptr);
QString algorithmBackend();
QString halconStatus();
} // namespace ivs
