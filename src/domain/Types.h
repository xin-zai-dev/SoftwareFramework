#pragma once

#include <QDateTime>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QRectF>
#include <QSize>
#include <QString>

namespace ivs {

struct DatabaseConfig {
    QString driver = QStringLiteral("QSQLITE");
    QString database = QStringLiteral("data/inspection.sqlite");
    QString host = QStringLiteral("localhost");
    int port = 1433;
    QString user;
    QString passwordEnvironment = QStringLiteral("IVS_SQLSERVER_PASSWORD");
    QString odbcDriver = QStringLiteral("ODBC Driver 18 for SQL Server");
    bool trustServerCertificate = false;
};

struct DeviceConfig {
    QString id;
    QString category;
    QString model;
    QString backend = QStringLiteral("simulated");
    QString host = QStringLiteral("127.0.0.1");
    int port = 502;
    int unitId = 1;
    QJsonObject parameters;
};

struct FlowNode {
    QString id;
    QString type;
    QString title;
    bool enabled = true;
    QJsonObject parameters;
};

struct PanelConfig {
    QString id;
    QString type;
    QString title;
    QString field = QStringLiteral("diameterMm");
};

struct ProjectConfig {
    int schemaVersion = 1;
    QString name = QStringLiteral("单工位尺寸检测示范");
    QString recipe = QStringLiteral("配方 1.0");
    QSize resolution = QSize(2448, 2048);
    int cycleMs = 1000;
    double pixelsPerMm = 20.0;
    double minDiameterMm = 19.0;
    double maxDiameterMm = 21.0;
    QString imagePolicy = QStringLiteral("ng");
    int imageRetentionDays = 30;
    qint64 imageQuotaMB = 1024;
    DatabaseConfig database;
    QList<DeviceConfig> devices;
    QList<FlowNode> flow;
    QList<PanelConfig> panels;
};

struct DetectionRecord {
    qint64 id = 0;
    QString runId;
    QString workpieceId;
    QDateTime timestamp;
    QString recipe;
    QString outcome;
    double diameterMm = 0;
    double offsetXmm = 0;
    double offsetYmm = 0;
    double durationMs = 0;
    QString imagePath;
    QString message;
};

struct RecordFilter {
    QString workpiece;
    QString outcome; // empty means all
    QDateTime from;
    QDateTime to;
    int limit = 100;
    qint64 beforeId = 0;
};

struct RecordSummary {
    qint64 total = 0;
    qint64 ok = 0;
    qint64 ng = 0;
    qint64 error = 0;
};

struct InspectionResult {
    DetectionRecord record;
    QImage frame;
    QRectF objectBounds;
    double diameterPixels = 0;
};

inline double recordField(const DetectionRecord &record, const QString &field) {
    if (field == QLatin1String("offsetXmm"))
        return record.offsetXmm;
    if (field == QLatin1String("offsetYmm"))
        return record.offsetYmm;
    if (field == QLatin1String("durationMs"))
        return record.durationMs;
    return record.diameterMm;
}

} // namespace ivs

Q_DECLARE_METATYPE(ivs::ProjectConfig)
Q_DECLARE_METATYPE(ivs::DetectionRecord)
Q_DECLARE_METATYPE(ivs::InspectionResult)
Q_DECLARE_METATYPE(QList<ivs::DetectionRecord>)
